// Keep the console only when there's something to say: debug builds (verbose
// hook logs) or recon builds (probe telemetry). Pure release stubs it so no
// messages are dispatched through the GumJS message pipe.
var COYOTE_DEBUG = (typeof globalThis.COYOTE_DEBUG !== 'undefined');
var COYOTE_RECON = (typeof globalThis.COYOTE_RECON !== 'undefined');
if (!COYOTE_DEBUG && !COYOTE_RECON) {
    console.log = function () {};
    console.error = function () {};
    console.warn = function () {};
}

// Recon telemetry: in recon builds, record a probe the target made. Prefixed so
// it's easy to filter (`adb logcat -s "[PuppyJS]" | grep RECON`).
function recon(vector, detail) {
    if (COYOTE_RECON) {
        console.log('[RECON] ' + vector + (detail !== undefined ? ': ' + detail : ''));
    }
}

console.log('[+] JSAgent: Starting...');

// Pure helper (hoisted from the old BLOCK 2) so staged hook blocks in
// either phase can use it without sharing closure scope.
function getRealisticJitter(baseValue, variancePercent) {
    const variance = baseValue * variancePercent;
    const offset = (Math.random() * variance * 2) - variance;
    return Math.floor(baseValue + offset);
}

// Random fixed-length string over an explicit alphabet. One helper for every
// generated identifier (hex Android IDs, numeric telephony IDs, package-name
// hashes); each caller passes the alphabet it needs. Uniform per character.
function randomString(len, alphabet) {
    let out = '';
    for (let i = 0; i < len; i++) {
        out += alphabet.charAt(Math.floor(Math.random() * alphabet.length));
    }
    return out;
}

// Run one hook family, isolating its failure so a single bad block can never
// abort the rest of the install.
function installBlock(label, fn) {
    try {
        fn();
    } catch (e) {
        console.error('[!] JSAgent: hook block "' + label + '" failed: ' + e);
    }
}

// Staged install. Installing all ~70 Java method replacements at once inside
// callActivityOnCreate ran ~70 ART deoptimizations back-to-back on the splash's
// critical path; each deopt suspends every thread in the process. On a trivial
// test app (few threads) that was merely slow; on real apps doing heavy work on
// background threads during the splash it ANR'd or deadlocked, so they never got
// past the splash screen. We now install only the detection-critical families
// synchronously, then drain the rest one-per-main-thread-tick once the splash is
// already up, so the remaining suspends are spread across frames off the
// critical path.
function installStaged(Java, blocks, critical, deferred) {
    var t0 = Date.now();
    for (var i = 0; i < critical.length; i++) {
        var name = critical[i];
        if (blocks[name]) installBlock(name, blocks[name]);
    }
    console.log('[+] JSAgent: ' + critical.length + ' critical hook block(s) installed in ' + (Date.now() - t0) + 'ms');

    var queue = deferred.slice();
    var canSchedule = (typeof Java.scheduleOnMainThread === 'function');

    var pump = function () {
        if (queue.length === 0) {
            console.log('[+] JSAgent: all deferred hook blocks installed');
            return;
        }
        var name = queue.shift();
        if (blocks[name]) installBlock(name, blocks[name]);
        // Re-post as a fresh main-loop message so the UI and the app's own
        // bootstrap run between blocks instead of one long main-thread stall.
        if (canSchedule) Java.scheduleOnMainThread(pump);
        else pump();
    };

    if (canSchedule) {
        Java.scheduleOnMainThread(pump);
    } else {
        console.log('[!] JSAgent: scheduleOnMainThread unavailable; draining deferred blocks synchronously');
        pump();
    }
}

function onJavaReady(Java) {
    console.log('[+] JSAgent: Java is ready! Attaching Java hooks...');

    // Each hook family is wrapped so the installer can stage it. The closures
    // capture `Java` and run exactly the original block code when invoked.
    var BLOCKS = {
        'build': function () {
    // ============================================
    // BLOCK 19: android.os.Build static field spoofing
    // ============================================
    try {
        const Build = Java.use('android.os.Build');
        const setBuildField = function (name, val) {
            if (typeof val === 'undefined') return;
            try {
                Build[name].value = val;
            } catch (e) {
                console.log('[-] Build.' + name + ': ' + e);
            }
        };
        setBuildField('SERIAL', global.DEVICE_SERIAL);
        setBuildField('FINGERPRINT', global.DEVICE_FINGERPRINT);
        setBuildField('MANUFACTURER', global.DEVICE_MANUFACTURER);
        setBuildField('BRAND', global.DEVICE_BRAND);
        setBuildField('MODEL', global.DEVICE_MODEL);
        setBuildField('DEVICE', global.DEVICE_DEVICE);
        setBuildField('BOARD', global.DEVICE_BOARD);
        setBuildField('HARDWARE', global.DEVICE_HARDWARE);
        console.log('[+] JSAgent spoofed android.os.Build static fields');
    } catch (e) {
        console.log('[-] Error spoofing Build fields: ' + e);
    }

    // NOTE: boot/uptime spoofing via SystemClock.elapsedRealtime/uptimeMillis
    // was removed, those clocks are called constantly by the framework's
    // render/input loop, and a per-call Java hook freezes the app. If uptime
    // realism is needed, do it at the native layer (spoof reads of /proc/uptime)
    // instead of hooking the hot managed clocks.
        },
        'getprop': function () {
    // ============================================
    // BLOCK 20: Shell getprop interception (Java layer)
    // ============================================
    try {
        const ProcessBuilder = Java.use('java.lang.ProcessBuilder');
        const SystemProperties = Java.use('android.os.SystemProperties');
        const ArrayList = Java.use('java.util.ArrayList');

        const extractGetpropKey = function (cmdList) {
            const n = cmdList.size();
            if (n === 0) return null;
            const c0 = cmdList.get(0).toString();
            const base0 = c0.substring(c0.lastIndexOf('/') + 1);
            if (base0 === 'getprop' && n >= 2) {
                return cmdList.get(1).toString();
            }
            for (let i = 0; i < n; i++) {
                const ci = cmdList.get(i).toString();
                const idx = ci.indexOf('getprop ');
                if (idx !== -1) {
                    let rest = ci.substring(idx + 8).trim();
                    const sp = rest.search(/[\s"'|&;>]/);
                    if (sp !== -1) rest = rest.substring(0, sp);
                    if (rest.length > 0) return rest;
                }
            }
            return null;
        };

        ProcessBuilder.start.implementation = function () {
            try {
                const key = extractGetpropKey(this.command());
                if (key !== null) {
                    recon('exec getprop', key);
                    const val = SystemProperties.get(key);
                    const newCmd = ArrayList.$new();
                    newCmd.add('/system/bin/echo');
                    newCmd.add(val !== null ? val : '');
                    this.command(newCmd);
                }
            } catch (e) {
                console.log('[-] getprop rewrite error: ' + e);
            }
            return this.start();
        };

        console.log('[+] JSAgent hooked ProcessBuilder getprop interception');
    } catch (e) {
        console.log('[-] Error hooking ProcessBuilder: ' + e);
    }
        },
        'debug_settings': function () {
    // ============================================
    // BLOCK 10: Development & Debug Settings Bypass
    // ============================================
    try {
        const genHex = function(len) { return randomString(len, '0123456789abcdef'); };

        const fakeId = genHex(16);
        const settingsMap = {
            'android_id': fakeId,
            'mobile_data_always_on': '0',
            'adb_enabled': '0',
            'adb_wifi_enabled': '0',
            'development_settings_enabled': '0',
            'install_non_market_apps': '0'
        };

        if (typeof global.SYS_PROP_RO_PRODUCT_MODEL !== 'undefined') {
            settingsMap['device_name'] = global.SYS_PROP_RO_PRODUCT_MODEL;
        }

        if (typeof global.SYS_PROP_RO_BUILD_DISPLAY_ID !== 'undefined') {
            settingsMap['database_creation_buildid'] = global.SYS_PROP_RO_BUILD_DISPLAY_ID;
        }

        if (typeof global.SYS_PROP_PERSIST_SYS_LOCALE !== 'undefined') {
            settingsMap['system_locales'] = global.SYS_PROP_PERSIST_SYS_LOCALE;
        }

        if (typeof global.NETWORK_NET_TYPE !== 'undefined') {
            settingsMap['wifi_on'] = (global.NETWORK_NET_TYPE === 1) ? '1' : '0';
            settingsMap['mobile_data'] = (global.NETWORK_NET_TYPE === 0) ? '1' : '0';
        }

        if (typeof global.DEVICE_KEYBOARD !== 'undefined') {
            settingsMap['default_input_method'] = global.DEVICE_KEYBOARD;
        }

        const classConfigs = {
            'android.provider.Settings$Global': ['device_name', 'mobile_data', 'mobile_data_always_on', 'database_creation_buildid', 'adb_wifi_enabled', 'development_settings_enabled', 'adb_enabled', 'install_non_market_apps', 'wifi_on'],
            'android.provider.Settings$Secure': ['android_id', 'mobile_data', 'development_settings_enabled', 'adb_enabled', 'install_non_market_apps', 'wifi_on', 'default_input_method'],
            'android.provider.Settings$System': ['system_locales', 'android_id', 'adb_enabled', 'install_non_market_apps', 'wifi_on']
        };

        Object.keys(classConfigs).forEach(function(className) {
            const clazz = Java.use(className);

            // Settings.get* is read constantly during startup. Precompute this
            // class's spoofed string/int values once so the hot hook is a single
            // O(1) object lookup instead of an Array.indexOf scan (+ parseInt)
            // per call. Membership in these maps is exactly the old
            // `indexOf(name) !== -1 && settingsMap[name] !== undefined` test.
            const strMap = {};
            const intMap = {};
            classConfigs[className].forEach(function(k) {
                if (settingsMap[k] !== undefined) {
                    strMap[k] = settingsMap[k];
                    intMap[k] = parseInt(settingsMap[k], 10);
                }
            });

            if (clazz.getString) {
                clazz.getString.overloads.forEach(function(overload) {
                    overload.implementation = function() {
                        const v = strMap[arguments[1]];
                        if (v !== undefined) return v;
                        return this.getString.apply(this, arguments);
                    };
                });
            }

            if (clazz.getInt) {
                clazz.getInt.overloads.forEach(function(overload) {
                    overload.implementation = function() {
                        const v = intMap[arguments[1]];
                        if (v !== undefined) return v;
                        return this.getInt.apply(this, arguments);
                    };
                });
            }
        });

        const VMDebug = Java.use('dalvik.system.VMDebug');
        if (VMDebug.lastDebuggerActivity) {
            VMDebug.lastDebuggerActivity.overload().implementation = function() {
                return -1;
            };
        }

        const Debug = Java.use('android.os.Debug');
        if (Debug.isDebuggerConnected) {
            Debug.isDebuggerConnected.implementation = function() {
                return false;
            };
        }

        console.log('[+] JSAgent hooked debug settings successfully');
    } catch (e) {
        console.log('[-] Error hooking debug settings: ' + e);
    }
        },
        'telephony': function () {
    // ============================================
    // BLOCK 9: Telephony Carrier Cloaking Hooks
    // ============================================
    if (typeof global.TELECOM_MCC !== 'undefined' ||
        typeof global.TELECOM_MNC !== 'undefined' ||
        typeof global.TELECOM_OPERATOR_NAME !== 'undefined' ||
        typeof global.TELECOM_COUNTRY_ISO !== 'undefined' ||
        typeof global.TELECOM_LANGUAGE !== 'undefined' ||
        typeof global.TELECOM_PHONE_CODE !== 'undefined' ||
        typeof global.TELECOM_SIM_STATE_NUM !== 'undefined' ||
        typeof global.TELECOM_MCC_COUNTRY_NAME !== 'undefined' ||
        typeof global.TELECOM_IMEI_PREFIX !== 'undefined' ||
        typeof global.TELECOM_NETWORK_TYPE !== 'undefined') {

        try {
            const TelephonyManager = Java.use('android.telephony.TelephonyManager');

            const randomStr = function(len) { return randomString(len, '0123456789'); };

            const telMap = {};

            if (typeof global.TELECOM_MCC !== 'undefined' && typeof global.TELECOM_MNC !== 'undefined') {
                const mccmnc = global.TELECOM_MCC + global.TELECOM_MNC;
                const subscriberId = mccmnc + randomStr(15 - mccmnc.length);
                telMap['getSimOperator'] = mccmnc;
                telMap['getNetworkOperator'] = mccmnc;
                telMap['getSubscriberId'] = subscriberId;
            }

            if (typeof global.TELECOM_OPERATOR_NAME !== 'undefined') {
                telMap['getSimOperatorName'] = global.TELECOM_OPERATOR_NAME;
                telMap['getNetworkOperatorName'] = global.TELECOM_OPERATOR_NAME;
            }

            if (typeof global.TELECOM_COUNTRY_ISO !== 'undefined') {
                telMap['getSimCountryIso'] = global.TELECOM_COUNTRY_ISO;
                telMap['getNetworkCountryIso'] = global.TELECOM_COUNTRY_ISO;
            }

            if (typeof global.TELECOM_PHONE_CODE !== 'undefined') {
                telMap['getLine1Number'] = global.TELECOM_PHONE_CODE + randomStr(10);
            }

            if (typeof global.TELECOM_IMEI_PREFIX !== 'undefined') {
                telMap['getDeviceId'] = global.TELECOM_IMEI_PREFIX + randomStr(13);
            }

            Object.keys(telMap).forEach(function(method) {
                if (TelephonyManager[method]) {
                    TelephonyManager[method].overload().implementation = function() {
                        return telMap[method];
                    };
                }
            });

            if (typeof global.TELECOM_SIM_STATE_NUM !== 'undefined') {
                if (TelephonyManager.getSimState) {
                    TelephonyManager.getSimState.overload().implementation = function() {
                        return global.TELECOM_SIM_STATE_NUM;
                    };
                }
            }

            if (typeof global.TELECOM_NETWORK_TYPE !== 'undefined') {
                if (TelephonyManager.getNetworkType) {
                    TelephonyManager.getNetworkType.overload().implementation = function() {
                        const typeMap = {'LTE': 13, '5G': 20, '4G': 13, '3G': 3, '2G': 1};
                        return typeMap[global.TELECOM_NETWORK_TYPE] || 13;
                    };
                }
            }

            if (typeof global.TELECOM_MCC !== 'undefined' ||
                typeof global.TELECOM_MNC !== 'undefined' ||
                (typeof global.TELECOM_LANGUAGE !== 'undefined' && typeof global.TELECOM_COUNTRY_ISO !== 'undefined')) {

                const Locale = Java.use('java.util.Locale');
                [Java.use('android.view.ContextThemeWrapper'), Java.use('android.app.ContextImpl')].forEach(function(ctx) {
                    if (ctx && ctx.getResources) {
                        ctx.getResources.overload().implementation = function() {
                            const res = this.getResources();
                            const config = res.getConfiguration();

                            if (typeof global.TELECOM_MCC !== 'undefined') {
                                config.mcc.value = parseInt(global.TELECOM_MCC, 10);
                            }
                            if (typeof global.TELECOM_MNC !== 'undefined') {
                                config.mnc.value = parseInt(global.TELECOM_MNC, 10);
                            }
                            if (typeof global.TELECOM_LANGUAGE !== 'undefined' && typeof global.TELECOM_COUNTRY_ISO !== 'undefined') {
                                config.setLocale(Locale.$new(global.TELECOM_LANGUAGE, global.TELECOM_COUNTRY_ISO));
                            }
                            return res;
                        };
                    }
                });
            }

            console.log('[+] JSAgent hooked telephony carrier successfully');
        } catch (e) {
            console.log('[-] Error hooking telephony carrier: ' + e);
        }
    }
        },
        'ram': function () {
    // ============================================
    // BLOCK 3: RAM Memory Hooks
    // ============================================
    if (typeof global.SPECS_TOTAL_RAM !== 'undefined' ||
        typeof global.SPECS_RAM_THRESHOLD !== 'undefined' ||
        typeof global.SPECS_AVAIL_RAM !== 'undefined') {
        try {
            const ActivityManager = Java.use('android.app.ActivityManager');
            ActivityManager.getMemoryInfo.implementation = function(mem) {
                this.getMemoryInfo(mem);

                if (mem !== null) {
                    if (typeof global.SPECS_TOTAL_RAM !== 'undefined') {
                        mem.totalMem.value = global.SPECS_TOTAL_RAM;
                    }

                    if (typeof global.SPECS_RAM_THRESHOLD !== 'undefined') {
                        mem.threshold.value = global.SPECS_RAM_THRESHOLD;
                    }

                    if (typeof global.SPECS_AVAIL_RAM !== 'undefined') {
                        mem.availMem.value = getRealisticJitter(global.SPECS_AVAIL_RAM, 0.03);
                    }

                    mem.lowMemory.value = false;
                }
            };
            console.log('[+] JSAgent hooked ActivityManager MemoryInfo successfully');
        } catch (e) {
            console.log('[-] Error hooking ActivityManager MemoryInfo: ' + e);
        }
    }
        },
        'display': function () {
    // ============================================
    // BLOCK 1: Display Metrics Hooks
    // ============================================
    try {
        const Display = Java.use('android.view.Display');

        const patchMetrics = function(metrics) {
            if (metrics !== null) {
                if (typeof global.SPECS_WIDTH !== 'undefined') {
                    metrics.widthPixels.value = global.SPECS_WIDTH;
                }
                if (typeof global.SPECS_HEIGHT !== 'undefined') {
                    metrics.heightPixels.value = global.SPECS_HEIGHT;
                }
                if (typeof global.SPECS_DENSITY !== 'undefined') {
                    metrics.density.value = global.SPECS_DENSITY;
                }
                if (typeof global.SPECS_DENSITY_DPI !== 'undefined') {
                    metrics.densityDpi.value = global.SPECS_DENSITY_DPI;
                }
                if (typeof global.SPECS_XDPI !== 'undefined') {
                    metrics.xdpi.value = global.SPECS_XDPI;
                }
                if (typeof global.SPECS_YDPI !== 'undefined') {
                    metrics.ydpi.value = global.SPECS_YDPI;
                }
            }
        };

        if (typeof global.SPECS_WIDTH !== 'undefined' ||
            typeof global.SPECS_HEIGHT !== 'undefined' ||
            typeof global.SPECS_DENSITY !== 'undefined' ||
            typeof global.SPECS_DENSITY_DPI !== 'undefined' ||
            typeof global.SPECS_XDPI !== 'undefined' ||
            typeof global.SPECS_YDPI !== 'undefined') {

            Display.getRealMetrics.overload('android.util.DisplayMetrics').implementation = function(metrics) {
                this.getRealMetrics(metrics);
                patchMetrics(metrics);
            };

            Display.getMetrics.overload('android.util.DisplayMetrics').implementation = function(metrics) {
                this.getMetrics(metrics);
                patchMetrics(metrics);
            };

            try {
                const Resources = Java.use('android.content.res.Resources');
                Resources.getDisplayMetrics.implementation = function() {
                    const metrics = this.getDisplayMetrics();
                    patchMetrics(metrics);
                    return metrics;
                };
                console.log('[+] JSAgent hooked Resources.getDisplayMetrics successfully');
            } catch (e) {
                console.log('[-] Error hooking Resources.getDisplayMetrics: ' + e);
            }

            console.log('[+] JSAgent hooked display metrics successfully');
        }
    } catch (e) {
        console.log('[-] Error hooking Display metrics: ' + e);
    }
        },
        'locale_tz': function () {
    // ============================================
    // BLOCK 11: Locale & Timezone Cloaking Hooks
    // ============================================
    if (typeof global.SYS_PROP_PERSIST_SYS_LANGUAGE !== 'undefined' ||
        typeof global.SYS_PROP_PERSIST_SYS_LOCALE !== 'undefined' ||
        typeof global.SYS_PROP_PERSIST_SYS_TIMEZONE !== 'undefined') {

        try {
            if (typeof global.SYS_PROP_PERSIST_SYS_LANGUAGE !== 'undefined' ||
                typeof global.SYS_PROP_PERSIST_SYS_LOCALE !== 'undefined') {

                const language = global.SYS_PROP_PERSIST_SYS_LANGUAGE || 'en';
                const locale = global.SYS_PROP_PERSIST_SYS_LOCALE || 'en-US';

                const Locale = Java.use('java.util.Locale');
                const country = locale.split('-')[1] || locale.toUpperCase();

                Locale.getDefault.overload().implementation = function() {
                    return Locale.$new(language, country);
                };

                const Build = Java.use('android.os.Build$VERSION');
                if (Build.SDK_INT.value >= 24) {
                    const LocaleList = Java.use('android.os.LocaleList');
                    LocaleList.getDefault.implementation = function() {
                        return LocaleList.$new(Java.array('java.util.Locale', [Locale.$new(language, country)]));
                    };
                }
            }

            if (typeof global.SYS_PROP_PERSIST_SYS_TIMEZONE !== 'undefined') {
                const timezone = global.SYS_PROP_PERSIST_SYS_TIMEZONE;
                const TimeZone = Java.use('java.util.TimeZone');

                TimeZone.getDefault.implementation = function() {
                    return TimeZone.getTimeZone(timezone);
                };
            }

            console.log('[+] JSAgent hooked locale/timezone successfully');
        } catch (e) {
            console.log('[-] Error hooking locale/timezone: ' + e);
        }
    }
        },
        'sensors': function () {
    // ============================================
    // BLOCK 24: Sensor fabrication
    // Emulators expose few/no sensors, which is a strong tell. Fabricate a
    // plausible set of android.hardware.Sensor objects via the package-private
    // no-arg constructor + private field writes. All of it is defensive: if the
    // ART build doesn't allow the construction, we fall back to recon-only so we
    // never crash a getSensorList() call.
    // ============================================
    try {
        const SensorManager = Java.use('android.hardware.SensorManager');
        const Sensor = Java.use('android.hardware.Sensor');
        const ArrayList = Java.use('java.util.ArrayList');

        // type, name, vendor, maxRange, resolution, power(mA), minDelay(us)
        const SENSORS = [
            {type: 1,  name: 'LSM6DSO Accelerometer', vendor: 'STMicroelectronics', max: 78.4532,   res: 0.0023956, power: 0.15,   minDelay: 2404},
            {type: 4,  name: 'LSM6DSO Gyroscope',     vendor: 'STMicroelectronics', max: 34.906586, res: 0.0010653, power: 0.65,   minDelay: 2404},
            {type: 2,  name: 'AK09918 Magnetometer',  vendor: 'AKM',                max: 4912.0,     res: 0.15,      power: 0.1,    minDelay: 10000},
            {type: 5,  name: 'TMD4906 Light',         vendor: 'AMS',                max: 60000.0,    res: 1.0,       power: 0.12,   minDelay: 0},
            {type: 8,  name: 'TMD4906 Proximity',     vendor: 'AMS',                max: 5.0,        res: 1.0,       power: 0.12,   minDelay: 0},
            {type: 6,  name: 'LPS22HH Pressure',      vendor: 'STMicroelectronics', max: 1260.0,     res: 0.0002,    power: 0.0017, minDelay: 40000},
        ];

        const setField = function (obj, field, val) {
            try { obj[field].value = val; } catch (e) {}
        };
        const buildSensor = function (spec, handle) {
            const s = Sensor.$new(); // package-private Sensor()
            setField(s, 'mType', spec.type);
            setField(s, 'mName', spec.name);
            setField(s, 'mVendor', spec.vendor);
            setField(s, 'mVersion', 1);
            setField(s, 'mHandle', handle);
            setField(s, 'mMaxRange', spec.max);
            setField(s, 'mResolution', spec.res);
            setField(s, 'mPower', spec.power);
            setField(s, 'mMinDelay', spec.minDelay);
            setField(s, 'mMaxDelay', 1000000);
            setField(s, 'mFifoReservedEventCount', 0);
            setField(s, 'mFifoMaxEventCount', 0);
            setField(s, 'mRequiredPermission', '');
            setField(s, 'mFlags', 0);
            return s;
        };

        // Probe once; only install fabricating hooks if construction works.
        let probe = null;
        try { probe = buildSensor(SENSORS[0], 0); } catch (e) { probe = null; }

        if (probe !== null && probe.getType() === 1) {
            // Build fresh each call, Frida Java wrappers don't survive across
            // call frames, so caching them makes a later getType()/add() throw
            // (which would return null to Java and NPE the caller).
            SensorManager.getSensorList.overload('int').implementation = function (type) {
                recon('SensorManager.getSensorList', type);
                const list = ArrayList.$new();
                for (let i = 0; i < SENSORS.length; i++) {
                    if (type === -1 /* TYPE_ALL */ || SENSORS[i].type === type) {
                        try { list.add(buildSensor(SENSORS[i], i + 1)); } catch (e) {}
                    }
                }
                return list;
            };
            SensorManager.getDefaultSensor.overload('int').implementation = function (type) {
                recon('SensorManager.getDefaultSensor', type);
                for (let i = 0; i < SENSORS.length; i++) {
                    if (SENSORS[i].type === type) {
                        try { return buildSensor(SENSORS[i], i + 1); } catch (e) {}
                    }
                }
                return this.getDefaultSensor(type);
            };
            console.log('[+] JSAgent fabricated SensorManager sensors (' + SENSORS.length + ')');
        } else {
            // Fabrication not supported here, keep recon only, change nothing.
            SensorManager.getSensorList.overload('int').implementation = function (type) {
                recon('SensorManager.getSensorList', type);
                return this.getSensorList(type);
            };
            console.log('[-] Sensor fabrication unavailable on this ART; recon-only');
        }
    } catch (e) {
        console.log('[-] Error hooking SensorManager: ' + e);
    }
        },
        'package_manager': function () {
    // ============================================
    // BLOCK 17: Package Manager Spoofing & App Hiding
    // ============================================
    try {
        const hidePackages = [
            'com.guoshi.httpcanary', 'org.blokada.alarm.dnschanger', 'com.adguard.android.contentblocker',
            'com.protectstar.adblocker', 'tech.httptoolkit.android', 'tech.httptoolkit.android.v1',
            'com.guoshi.httpcanary.premium', 'com.hsv.freeadblockerbrowser', 's.sdownload.adblockerultimatebrowser',
            'com.egorovandreyrm.pcapremote', 'com.packagesniffer.frtparlak', 'jp.co.taosoftware.android.packetcapture',
            'app.greyshirts.sslcapture', 'com.minhui.networkcapture.pro', 'com.evbadroid.wicapdemo',
            'com.evbadroid.wicap', 'com.luckypatchers.luckypatcherinstaller', 'ru.UbLBBRLf.jSziIaUjL',
            'com.emanuelef.remote_capture', 'com.minhui.wifianalyzer', 'com.evbadroid.proxymon',
            'com.privateinternetaccess.android',
            // Root / Magisk / superuser / instrumentation managers
            'com.topjohnwu.magisk', 'io.github.vvb2060.magisk', 'io.github.huskydg.magisk',
            'eu.chainfire.supersu', 'com.koushikdutta.superuser', 'com.thirdparty.superuser',
            'com.noshufou.android.su', 'me.weishu.kernelsu', 'me.weishu.exp',
            'de.robv.android.xposed.installer', 'org.lsposed.manager',
            'com.saurik.substrate', 're.frida.server'
        ];

        const forcedFeatures = [
            'android.hardware.camera', 'android.hardware.camera.front', 'android.hardware.camera.flash',
            'android.hardware.bluetooth', 'android.hardware.bluetooth.le', 'android.hardware.nfc',
            'android.hardware.location', 'android.hardware.location.gps', 'android.hardware.microphone',
            'android.hardware.wifi', 'android.hardware.sensor.accelerometer', 'android.hardware.sensor.barometer',
            'android.hardware.sensor.compass', 'android.hardware.sensor.gyroscope', 'android.hardware.sensor.light',
            'android.hardware.sensor.proximity', 'android.hardware.telephony', 'android.hardware.touchscreen',
            'android.hardware.fingerprint', 'android.hardware.biometrics.face'
        ];

        const fakeApps = [
            {pkg: 'com.whatsapp', act: 'com.whatsapp.Main', ver: '2.26.7.74', code: 226077400},
            {pkg: 'com.facebook.katana', act: 'com.facebook.katana.LoginActivity', ver: '548.1.0.51.64', code: 548105164},
            {pkg: 'com.zhiliaoapp.musically', act: 'com.ss.android.ugc.aweme.splash.SplashActivity', ver: '43.9.3', code: 430903},
            {pkg: 'id.dana', act: 'id.dana.onboarding.splash.DanaSplashActivity', ver: '2.116.0', code: 211600},
            {pkg: 'com.zing.zalo', act: 'com.zing.zalo.ui.SplashActivity', ver: '26.02.01', code: 260201},
            {pkg: 'org.telegram.messenger', act: 'org.telegram.messenger.DefaultIcon', ver: '12.4.3', code: 124300}
        ];

        const JavaUtilList = Java.use('java.util.List');
        const PackageManager = Java.use('android.content.pm.PackageManager');
        const ApplicationInfo = Java.use('android.content.pm.ApplicationInfo');
        const PackageInfo = Java.use('android.content.pm.PackageInfo');

        const targetPackageName = global.PACKAGE_NAME || 'com.border.patrol';
        const cachedInstallTime = global.CACHED_INSTALL_TIME || 1609459200; // Fallback timestamp if Block 6 isn't present

        const fakePackageName = function (pname) {
            if (hidePackages.includes(pname)) {
                return 'com.android.vending.licensing.ILicensingService';
            }
            return pname;
        };

        function generateRandomHash(length) {
            return randomString(length, 'ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_');
        }

        function create_app_resolve_info(packageName, mainActivity) {
            let ActivityInfo = Java.use("android.content.pm.ActivityInfo");
            let ApplicationInfo = Java.use("android.content.pm.ApplicationInfo");
            let ResolveInfo = Java.use("android.content.pm.ResolveInfo");
            const appInfo = ApplicationInfo.$new();
            appInfo.packageName.value = packageName;
            let dirHash = generateRandomHash(22);
            let pkgHash = generateRandomHash(22);
            let fakePath = "/data/app/~~" + dirHash + "==/" + packageName + "-" + pkgHash + "==/base.apk";
            appInfo.sourceDir.value = fakePath;
            appInfo.publicSourceDir.value = fakePath;
            const actInfo = ActivityInfo.$new();
            actInfo.name.value = mainActivity;
            actInfo.packageName.value = packageName;
            actInfo.applicationInfo.value = appInfo;
            console.log("[+] Created ActivityInfo for: " + actInfo.name.value);
            let rInfo = ResolveInfo.$new();
            rInfo.activityInfo.value = actInfo;
            return rInfo;
        }

        function create_app_package_info(packageName, mainActivity, versionName, versionCode) {
            try {
                let PackageInfo = Java.use("android.content.pm.PackageInfo");
                let ApplicationInfo = Java.use("android.content.pm.ApplicationInfo");
                let ActivityInfo = Java.use("android.content.pm.ActivityInfo");

                const appInfo = ApplicationInfo.$new();
                appInfo.packageName.value = packageName;

                let dirHash = generateRandomHash(22);
                let pkgHash = generateRandomHash(22);
                let fakePath = "/data/app/~~" + dirHash + "==/" + packageName + "-" + pkgHash + "==/base.apk";
                appInfo.sourceDir.value = fakePath;
                appInfo.publicSourceDir.value = fakePath;

                const actInfo = ActivityInfo.$new();
                actInfo.name.value = mainActivity;
                actInfo.packageName.value = packageName;
                actInfo.applicationInfo.value = appInfo;

                const pkgInfo = PackageInfo.$new();

                pkgInfo.packageName.value = packageName;
                pkgInfo.versionName.value = versionName;
                pkgInfo.versionCode.value = versionCode;

                pkgInfo.applicationInfo.value = appInfo;

                let activityArray = null;
                try {
                    activityArray = Java.array("android.content.pm.ActivityInfo", [actInfo]);
                    if (activityArray) {
                        pkgInfo.activities.value = activityArray;
                    }
                } catch (e) { }

                console.log("[+] Created PackageInfo for: " + pkgInfo.packageName.value + " (Version: " + pkgInfo.versionName.value + ")");

                return pkgInfo;
            } catch (e) {
                console.log('[-] Error creating package info: ' + e);
                return null;
            }
        }

        const filterInstalledApps = function (list) {
            try {
                const castedList = Java.cast(list, JavaUtilList);
                if (!castedList) return;

                for (let i = castedList.size() - 1; i >= 0; i--) {
                    try {
                        const pkg = Java.cast(castedList.get(i), ApplicationInfo).packageName.value;
                        if (hidePackages.includes(pkg)) {
                            castedList.remove(i);
                        }
                    } catch (e) {}
                }
            } catch (e) {
                console.log('[-] Error filtering: ' + e);
            }
        };

        const filterInstalledPackages = function (list) {
            const castedList = Java.cast(list, JavaUtilList);
            for (let i = castedList.size() - 1; i >= 0; i--) {
                try {
                    const pkg = Java.cast(castedList.get(i), PackageInfo).packageName.value;
                    if (hidePackages.includes(pkg) || pkg.includes('lineageos')) {
                        list.remove(i);
                    }
                } catch (e) {}
            }
            for (let j = 0; j < fakeApps.length; j++) {
                try {
                    const fakeInfo = create_app_package_info(fakeApps[j].pkg, fakeApps[j].act, fakeApps[j].ver, fakeApps[j].code);
                    list.add(fakeInfo);
                } catch (e) {}
            }
        };

        PackageManager.getInstalledApplications.overload('int').implementation = function (flags) {
            const list = this.getInstalledApplications(flags);
            filterInstalledApps(list);
            return list;
        };

        PackageManager.getInstalledPackages.overload('int').implementation = function (flags) {
            const list = this.getInstalledPackages(flags);
            filterInstalledPackages(list);
            return list;
        };

        try {
            const ApplicationPackageManager = Java.use('android.app.ApplicationPackageManager');
            ApplicationPackageManager.getInstallerPackageName.overload('java.lang.String').implementation = function (pkg) {
                recon('getInstallerPackageName', pkg);
                if (pkg === targetPackageName) {
                    return 'com.android.vending';
                }
                const real = this.getInstallerPackageName(pkg);
                return (real === null) ? 'com.android.vending' : real;
            };
            console.log('[+] JSAgent hooked getInstallerPackageName (ApplicationPackageManager)');
        } catch (e) {
            console.log('[-] Error hooking getInstallerPackageName: ' + e);
        }

        try {
            const InstallSourceInfo = Java.use('android.content.pm.InstallSourceInfo');
            InstallSourceInfo.getInitiatingPackageName.implementation = function () {
                return 'com.android.vending';
            };
            InstallSourceInfo.getInstallingPackageName.implementation = function () {
                return 'com.android.vending';
            };

            console.log('[+] JSAgent hooked InstallSourceInfo successfully');
        } catch (e) {
            console.log('[-] Error hooking InstallSourceInfo: ' + e);
        }

        if (PackageManager.hasSystemFeature && PackageManager.hasSystemFeature.overloads) {
            for (let i = 0; i < PackageManager.hasSystemFeature.overloads.length; i++) {
                try {
                    PackageManager.hasSystemFeature.overloads[i].implementation = function () {
                        const feature = arguments[0];
                        if (feature && forcedFeatures.includes(feature)) return true;
                        return this.hasSystemFeature.apply(this, arguments);
                    };
                } catch (e) {
                    console.log('[-] Error hooking overload ' + i + ': ' + e);
                }
            }
        }

        PackageManager.getPackageInfo.overload('java.lang.String', 'int').implementation = function (pname, flags) {
            const info = this.getPackageInfo(fakePackageName(pname), flags);
            const spoofedTime = cachedInstallTime * 1000;
            info.firstInstallTime.value = spoofedTime;
            if (info.lastUpdateTime.value < spoofedTime) {
                info.lastUpdateTime.value = spoofedTime;
            }
            return info;
        };

        console.log('[+] JSAgent spoofing package manager successfully');
    } catch (e) {
        console.log('[-] Error in package manager spoofing block: ' + e);
    }
        },
        'ssl': function () {
    // ============================================
    // BLOCK 15: SSL Pinning Bypass Hooks
    // ============================================
    const optionalClass = function (name) {
        try {
            return Java.use(name);
        } catch (e) {
            if (('' + e).indexOf('ClassNotFound') === -1) {
                console.log('[-] Unexpected error loading ' + name + ': ' + e);
            }
            return null;
        }
    };

    const hookSsl = function (className, label, fn) {
        const clazz = optionalClass(className);
        if (!clazz) return;
        try {
            fn(clazz);
            console.log('[+] JSAgent hooked ' + label + ' successfully');
        } catch (e) {
            console.log('[-] Error hooking ' + label + ': ' + e);
        }
    };

    hookSsl('okhttp3.CertificatePinner', 'okhttp3', function (CertificatePinner) {
        CertificatePinner.check.overload('java.lang.String', 'java.util.List').implementation = function (str) {
            return;
        };
    });

    hookSsl('com.datatheorem.android.trustkit.pinning.OkHostnameVerifier', 'trustkit', function (OkHostnameVerifier) {
        OkHostnameVerifier.verify.overload('java.lang.String', 'javax.net.ssl.SSLSession').implementation = function () {
            return true;
        };
        OkHostnameVerifier.verify.overload('java.lang.String', 'java.security.cert.X509Certificate').implementation = function () {
            return true;
        };
    });

    hookSsl('com.android.org.conscrypt.TrustManagerImpl', 'TrustManagerImpl', function (TrustManagerImpl) {
        TrustManagerImpl.verifyChain.implementation = function (untrustedChain) {
            return untrustedChain;
        };
    });

    hookSsl('appcelerator.https.PinningTrustManager', 'Appcelerator', function (PinningTrustManager) {
        PinningTrustManager.checkServerTrusted.implementation = function () {
            return;
        };
    });
        },
        'user_agent': function () {
    // ============================================
    // BLOCK 16: User-Agent Spoofing Hooks
    // ============================================
    if (typeof global.DEVICE_MODEL !== 'undefined' && typeof global.DEVICE_DISPLAY_ID !== 'undefined') {
        try {
            const webViewPrefix = 'Mozilla/5.0';
            const nativePrefix = 'Dalvik/2.1.0';

            const baseOS = 'Linux; Android 16; ' + global.DEVICE_MODEL + ' Build/' + global.DEVICE_DISPLAY_ID;
            const UA = ' (' + baseOS + ')';
            const UAW = ' (' + baseOS + '; wv) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/131.0.6778.200 Mobile Safari/537.36';

            // System.getProperty is one of the hottest methods in the VM. Only
            // the single-String overload carries 'http.agent', so hook just that
            // one (not the (String,String) overload) to halve the footprint.
            const System = Java.use('java.lang.System');
            try {
                System.getProperty.overload('java.lang.String').implementation = function (name) {
                    if (name === 'http.agent') {
                        return nativePrefix + UA;
                    }
                    return System.getProperty.apply(this, arguments);
                };
            } catch (e) {
                console.log('[-] Error hooking System.getProperty: ' + e);
            }

            try {
                const WebView = Java.use('android.webkit.WebView');
                WebView.getSettings.implementation = function () {
                    const settings = this.getSettings();
                    settings.setUserAgentString(webViewPrefix + UAW);
                    return settings;
                };
            } catch (e) {
                console.log('[-] Error hooking WebView: ' + e);
            }

            try {
                const URL = Java.use('java.net.URL');
                const HttpURLConnection = Java.use('java.net.HttpURLConnection');

                URL.openConnection.overload().implementation = function () {
                    const conn = this.openConnection();
                    if (HttpURLConnection.class.isInstance(conn)) {
                        Java.cast(conn, HttpURLConnection).setRequestProperty('User-Agent', nativePrefix + UA);
                    }
                    return conn;
                };

                URL.openConnection.overload('java.net.Proxy').implementation = function (proxy) {
                    const conn = this.openConnection(proxy);
                    if (HttpURLConnection.class.isInstance(conn)) {
                        Java.cast(conn, HttpURLConnection).setRequestProperty('User-Agent', nativePrefix + UA);
                    }
                    return conn;
                };
            } catch (e) {
                console.log('[-] Error hooking URL: ' + e);
            }

            console.log('[+] JSAgent spoofing User-Agent successfully');
        } catch (e) {
            console.log('[-] Error in User-Agent spoofing block: ' + e);
        }
    }
        },
        'network_vpn': function () {
    // ============================================
    // BLOCK 13: Network & VPN Bypass Hooks
    // ============================================
    if (typeof global.NETWORK_NET_TYPE !== 'undefined' ||
        typeof global.NETWORK_MOBILE_DATA_LOCAL_IP !== 'undefined' ||
        typeof global.NETWORK_WIFI_LOCAL_IP !== 'undefined') {

        try {
            if (typeof global.NETWORK_NET_TYPE !== 'undefined') {
                const TRANSPORT_CELLULAR = 0, TRANSPORT_WIFI = 1, TRANSPORT_VPN = 4;
                const CAP_INTERNET = 12, CAP_NOT_VPN = 15, CAP_VALIDATED = 16;

                const NetworkInfo = Java.use('android.net.NetworkInfo');
                const NetworkInfoState = Java.use('android.net.NetworkInfo$State');
                const NetworkInfoDetailedState = Java.use('android.net.NetworkInfo$DetailedState');
                const ConnectivityManager = Java.use('android.net.ConnectivityManager');
                const NetworkCapabilities = Java.use('android.net.NetworkCapabilities');
                const Builder = Java.use('android.net.NetworkCapabilities$Builder');
                const TelephonyManager = Java.use('android.telephony.TelephonyManager');

                const netType = global.NETWORK_NET_TYPE;

                const createFakeNetInfo = function() {
                    const type = netType === 1 ? 'WIFI' : 'MOBILE';
                    const sub = netType === 1 ? '' : 'LTE';
                    return NetworkInfo.$new(netType, 0, type, sub);
                };

                NetworkCapabilities.hasTransport.implementation = function(t) {
                    if (t === TRANSPORT_VPN) return false;
                    if (t === TRANSPORT_WIFI && netType === 0) return false;
                    if (t === TRANSPORT_CELLULAR && netType === 1) return false;
                    return this.hasTransport(t);
                };

                ConnectivityManager.isActiveNetworkMetered.implementation = function() {
                    return netType === 0;
                };

                NetworkInfo.isConnected.implementation = function() {
                    return netType === this.getType();
                };

                NetworkInfo.isConnectedOrConnecting.implementation = function() {
                    return netType === this.getType();
                };

                NetworkInfo.getState.implementation = function() {
                    return netType === this.getType() ? NetworkInfoState.CONNECTED.value : NetworkInfoState.DISCONNECTED.value;
                };

                NetworkInfo.getDetailedState.implementation = function() {
                    return netType === this.getType() ? NetworkInfoDetailedState.CONNECTED.value : NetworkInfoDetailedState.DISCONNECTED.value;
                };

                ConnectivityManager.getActiveNetworkInfo.implementation = function() {
                    return createFakeNetInfo();
                };

                TelephonyManager.isDataEnabled.implementation = function() {
                    return netType === 0;
                };

                TelephonyManager.getDataState.implementation = function() {
                    return netType === 0 ? 2 : this.getDataState();
                };

                ConnectivityManager.getAllNetworks.implementation = function() {
                    const active = this.getActiveNetwork();
                    return active != null ? Java.array('android.net.Network', [active]) : this.getAllNetworks();
                };

                ConnectivityManager.getNetworkCapabilities.implementation = function(network) {
                    const builder = Builder.$new();
                    if (netType === 0) {
                        builder.addTransportType(TRANSPORT_CELLULAR);
                        builder.removeTransportType(TRANSPORT_WIFI);
                    } else {
                        builder.addTransportType(TRANSPORT_WIFI);
                        builder.removeTransportType(TRANSPORT_CELLULAR);
                    }
                    builder.removeTransportType(TRANSPORT_VPN);
                    builder.addCapability(CAP_INTERNET);
                    builder.addCapability(CAP_NOT_VPN);
                    builder.addCapability(CAP_VALIDATED);
                    return builder.build();
                };
            }

            if (typeof global.NETWORK_MOBILE_DATA_LOCAL_IP !== 'undefined' ||
                typeof global.NETWORK_WIFI_LOCAL_IP !== 'undefined') {

                const NetworkInterface = Java.use('java.net.NetworkInterface');
                const InetAddress = Java.use('java.net.InetAddress');
                const Vector = Java.use('java.util.Vector');

                const mobileIp = global.NETWORK_MOBILE_DATA_LOCAL_IP || null;
                const wifiIp = global.NETWORK_WIFI_LOCAL_IP || null;

                const isMobileActive = (typeof global.NETWORK_NET_TYPE !== 'undefined' && global.NETWORK_NET_TYPE === 0);
                const isWifiActive = (typeof global.NETWORK_NET_TYPE !== 'undefined' && global.NETWORK_NET_TYPE === 1);

                const createInterface = function(name, id, ip, hasIp) {
                    return hasIp ? NetworkInterface.$new(name, id, [InetAddress.getByName(ip)]) : NetworkInterface.$new(name, id, []);
                };

                NetworkInterface.getNetworkInterfaces.implementation = function() {
                    const realEnum = this.getNetworkInterfaces();
                    const list = Vector.$new();
                    let mobileFound = false, wifiFound = false;

                    while (realEnum.hasMoreElements()) {
                        const iface = Java.cast(realEnum.nextElement(), NetworkInterface);
                        const name = iface.getName();

                        if (name && (name.indexOf('tun') < 0 && name.indexOf('ppp') < 0)) {
                            if (name.startsWith('rmnet_data0')) {
                                mobileFound = true;
                                if (mobileIp !== null) list.add(createInterface(name, 100, mobileIp, isMobileActive));
                                else list.add(iface);
                            } else if (name.startsWith('wlan0')) {
                                wifiFound = true;
                                if (wifiIp !== null) list.add(createInterface(name, 102, wifiIp, isWifiActive));
                                else list.add(iface);
                            } else {
                                list.add(iface);
                            }
                        }
                    }

                    if (!mobileFound && mobileIp !== null) list.add(createInterface('rmnet_data0', 100, mobileIp, isMobileActive));
                    if (!wifiFound && wifiIp !== null) list.add(createInterface('wlan0', 102, wifiIp, isWifiActive));

                    return list.elements();
                };
            }

            console.log('[+] JSAgent hooked network/VPN successfully');
        } catch (e) {
            console.log('[-] Error hooking network/VPN: ' + e);
        }
    }
        },
        'proxy': function () {
    // ============================================
    // BLOCK 14: Proxy Bypass Hook
    // ============================================
    try {
        const Proxy = Java.use('android.net.Proxy');
        Proxy.getDefaultHost.implementation = function () {
            return null;
        };

        console.log('[+] JSAgent hooked proxy successfully');
    } catch (e) {
        console.log('[-] Error hooking proxy: ' + e);
    }
        },
        'battery': function () {
    // ============================================
    // BLOCK 4: Battery Cloaking Hooks
    // ============================================
    if (typeof global.SPECS_BATTERY_LEVEL !== 'undefined') {
        try {
            const BatteryManager = Java.use('android.os.BatteryManager');
            const Intent = Java.use('android.content.Intent');
            const Bundle = Java.use('android.os.Bundle');

            const battery_level = global.SPECS_BATTERY_LEVEL;

            const jitter = Math.floor(Math.random() * 50) - 25;
            const spoof_voltage = Math.floor(3500 + (battery_level * 8.5) + jitter);
            const spoof_temp = Math.floor(Math.random() * 81) + 260;

            const getSpoofedBattery = function(id, val) {
                if (id === 4) return battery_level;
                if (id === 6) return 3;
                if (id === 2 || id === 3) return -450000;
                if ((id === 1 || id === 5) && val == 0) return (id === 1) ? 2000000 : 7000000000;
                return val;
            };

            if (BatteryManager.getIntProperty) {
                BatteryManager.getIntProperty.overload('int').implementation = function(id) {
                    return Number(getSpoofedBattery(id, this.getIntProperty(id)));
                };
            }

            if (BatteryManager.getLongProperty) {
                BatteryManager.getLongProperty.overload('int').implementation = function(id) {
                    return Number(getSpoofedBattery(id, this.getLongProperty(id)));
                };
            }

            if (BatteryManager.isCharging) {
                BatteryManager.isCharging.implementation = function() {
                    return false;
                };
            }

            if (BatteryManager.computeChargeTimeRemaining) {
                BatteryManager.computeChargeTimeRemaining.implementation = function() {
                    return -1;
                };
            }

            // Intent.getIntExtra and Bundle.getInt fire on every intent/bundle
            // read in the process, so this must be allocation-free: the old code
            // built a fresh object literal on each call. Pure-spoof keys return a
            // constant without even calling the original (getIntExtra/getInt are
            // side-effect-free getters); only the ret-dependent keys defer to it.
            const spoofBatteryInt = function(self, args, overload) {
                switch (args[0]) {
                    case 'plugged': return 0;
                    case 'status': return 3;
                    case 'level': return battery_level;
                    case 'health': return 2;
                    case 'scale': { const r = overload.apply(self, args); return r === 0 ? 100 : r; }
                    case 'voltage': { const r = overload.apply(self, args); return r === 0 ? spoof_voltage : r; }
                    case 'temperature': { const r = overload.apply(self, args); return r === 0 ? spoof_temp : r; }
                    default: return overload.apply(self, args);
                }
            };

            Intent.getIntExtra.overloads.forEach(function(overload) {
                overload.implementation = function() {
                    return spoofBatteryInt(this, arguments, overload);
                };
            });

            Bundle.getInt.overloads.forEach(function(overload) {
                overload.implementation = function() {
                    return spoofBatteryInt(this, arguments, overload);
                };
            });

            console.log('[+] JSAgent hooked battery info successfully');
        } catch (e) {
            console.log('[-] Error hooking battery info: ' + e);
        }
    }
        },
        'time': function () {
    // ============================================
    // BLOCK 5: Time Cloaking Hooks
    // ============================================
    if (typeof global.SYS_PROP_PERSIST_SYS_TIMEZONE !== 'undefined') {
        try {
            const Date = Java.use('java.util.Date');
            const Calendar = Java.use('java.util.Calendar');
            const TimeZone = Java.use('java.util.TimeZone');
            const timeZoneID = global.SYS_PROP_PERSIST_SYS_TIMEZONE;

            Date.after.overload('java.util.Date').implementation = function(date) {
                return true;
            };

            Calendar.getInstance.overload().implementation = function() {
                const cal = this.getInstance();
                cal.setTimeZone(TimeZone.getTimeZone(timeZoneID));
                return cal;
            };

            console.log('[+] JSAgent hooked time cloaking successfully');
        } catch (e) {
            console.log('[-] Error hooking time cloaking: ' + e);
        }
    }
        },
        'keyguard': function () {
    // ============================================
    // BLOCK 18: Keyguard Bypass
    // ============================================
    try {
        const KeyguardManager = Java.use('android.app.KeyguardManager');

        KeyguardManager.isKeyguardSecure.implementation = function () {
            return true;
        };

        for (let i = 0; i < KeyguardManager.isDeviceSecure.overloads.length; i++) {
            KeyguardManager.isDeviceSecure.overloads[i].implementation = function () {
                return true;
            };
        }

        console.log('[+] JSAgent hooked KeyguardManager successfully');
    } catch (e) {
        console.log('[-] Error hooking KeyguardManager: ' + e);
    }
        },
        'flag_secure': function () {
    // ============================================
    // BLOCK 8: FLAG_SECURE Bypass Hooks
    // ============================================
    if (typeof global.BYPASS_FLAG_SECURE !== 'undefined') {
        try {
            const Window = Java.use('android.view.Window');
            const LayoutParams = Java.use('android.view.WindowManager$LayoutParams');
            const FLAG_SECURE = 8192;

            Window.addFlags.overload('int').implementation = function(flag) {
                if (flag === FLAG_SECURE) return;
                this.addFlags(flag);
            };

            Window.setFlags.overload('int', 'int').implementation = function(flags, mask) {
                flags = flags & ~LayoutParams.FLAG_SECURE.value;
                this.setFlags(flags, mask);
            };

            console.log('[+] JSAgent hooked FLAG_SECURE successfully');
        } catch (e) {
            console.log('[-] Error hooking FLAG_SECURE: ' + e);
        }
    }
        },
        'usb': function () {
    // ============================================
    // BLOCK 7: USB Action Bypass Hooks
    // ============================================
    try {
        const Intent = Java.use('android.content.Intent');
        const usbAction = 'android.hardware.usb.action.USB_STATE';
        const usbKeys = {
            'connected': true,
            'adb': true,
            'configured': true
        };

        Intent.getExtras.overload().implementation = function() {
            const bundle = this.getExtras();

            if (this.getAction() === usbAction && bundle !== null) {
                bundle.putBoolean('connected', false);
                bundle.putBoolean('adb', false);
                bundle.putBoolean('configured', false);
            }

            return bundle;
        };

        Intent.getBooleanExtra.overload('java.lang.String', 'boolean').implementation = function(key, defaultVal) {
            if (this.getAction() === usbAction && usbKeys[key]) {
                return false;
            }

            return this.getBooleanExtra(key, defaultVal);
        };

        console.log('[+] JSAgent hooked USB action successfully');
    } catch (e) {
        console.log('[-] Error hooking USB action: ' + e);
    }
        },
        'install_referrer': function () {
    // ============================================
    // BLOCK 6: Install Referrer Bypass Hooks
    // ============================================
    if (typeof global.ATTRIBUTION_INSTALL_REFERRER !== 'undefined') {
        try {
            const installReferrer = global.ATTRIBUTION_INSTALL_REFERRER;

            const cachedInstallTime = 1609459200;
            const cachedClickTime = 1609459200;

            try {
                const ReferrerDetails = Java.use('com.android.installreferrer.api.ReferrerDetails');
                if (ReferrerDetails && ReferrerDetails.getInstallReferrer) {
                    ReferrerDetails.getInstallReferrer.implementation = function() {
                        return installReferrer;
                    };
                }
            } catch (e) {}

            const Bundle = Java.use('android.os.Bundle');

            // Bundle.get* is hot; check the key before touching the original and
            // hoist the timestamp map out of the per-call path (it was rebuilt on
            // every getLong before).
            Bundle.getString.overloads.forEach(function(overload) {
                overload.implementation = function() {
                    if (arguments[0] === 'install_referrer') return installReferrer;
                    return overload.apply(this, arguments);
                };
            });

            const timeMap = {
                'install_begin_timestamp_seconds': cachedInstallTime,
                'install_begin_timestamp_server_seconds': cachedInstallTime,
                'referrer_click_timestamp_seconds': cachedClickTime,
                'referrer_click_timestamp_server_seconds': cachedClickTime
            };

            Bundle.getLong.overloads.forEach(function(overload) {
                overload.implementation = function() {
                    const v = timeMap[arguments[0]];
                    if (v !== undefined) return v;
                    return overload.apply(this, arguments);
                };
            });

            console.log('[+] JSAgent hooked install referrer successfully');
        } catch (e) {
            console.log('[-] Error hooking install referrer: ' + e);
        }
    }
        },
        'accounts': function () {
    // ============================================
    // BLOCK 22: Accounts (a "lived-in" device has a Google account)
    // ============================================
    if (typeof global.IDENTITY_ACCOUNT_EMAIL !== 'undefined') {
        try {
            const AccountManager = Java.use('android.accounts.AccountManager');
            const Account = Java.use('android.accounts.Account'); // public ctor (name, type)
            const accType = (typeof global.IDENTITY_ACCOUNT_TYPE !== 'undefined')
                ? global.IDENTITY_ACCOUNT_TYPE : 'com.google';

            const makeAccounts = function () {
                return Java.array('android.accounts.Account', [
                    Account.$new(global.IDENTITY_ACCOUNT_EMAIL, accType)
                ]);
            };

            AccountManager.getAccounts.implementation = function () {
                recon('AccountManager.getAccounts');
                return makeAccounts();
            };
            if (AccountManager.getAccountsByType) {
                AccountManager.getAccountsByType.overload('java.lang.String').implementation = function (type) {
                    recon('AccountManager.getAccountsByType', type);
                    if (type === null || type === accType) return makeAccounts();
                    return Java.array('android.accounts.Account', []);
                };
            }
            console.log('[+] JSAgent hooked AccountManager successfully');
        } catch (e) {
            console.log('[-] Error hooking AccountManager: ' + e);
        }
    }
        },
        'wifi': function () {
    // ============================================
    // BLOCK 23: Wi-Fi identity (SSID / BSSID / MAC)
    // ============================================
    if (typeof global.IDENTITY_WIFI_SSID !== 'undefined' ||
        typeof global.IDENTITY_WIFI_BSSID !== 'undefined' ||
        typeof global.IDENTITY_WIFI_MAC !== 'undefined') {
        try {
            const WifiInfo = Java.use('android.net.wifi.WifiInfo');

            if (typeof global.IDENTITY_WIFI_SSID !== 'undefined' && WifiInfo.getSSID) {
                WifiInfo.getSSID.implementation = function () {
                    recon('WifiInfo.getSSID');
                    return '"' + global.IDENTITY_WIFI_SSID + '"'; // framework wraps SSID in quotes
                };
            }
            if (typeof global.IDENTITY_WIFI_BSSID !== 'undefined' && WifiInfo.getBSSID) {
                WifiInfo.getBSSID.implementation = function () {
                    recon('WifiInfo.getBSSID');
                    return global.IDENTITY_WIFI_BSSID;
                };
            }
            if (typeof global.IDENTITY_WIFI_MAC !== 'undefined' && WifiInfo.getMacAddress) {
                WifiInfo.getMacAddress.implementation = function () {
                    recon('WifiInfo.getMacAddress');
                    return global.IDENTITY_WIFI_MAC;
                };
            }
            console.log('[+] JSAgent hooked WifiInfo successfully');
        } catch (e) {
            console.log('[-] Error hooking WifiInfo: ' + e);
        }
    }
        },
        'location': function () {
    // ============================================
    // BLOCK 25: Location spoofing (GPS fix matching the profile)
    // Coordinates come from the profile's `location` section (explicit or
    // country-derived). Also returns a matching-country Address for apps that
    // reverse-geocode the fix.
    // ============================================
    if (typeof global.LOCATION_LATITUDE !== 'undefined' && typeof global.LOCATION_LONGITUDE !== 'undefined') {
        try {
            const Location = Java.use('android.location.Location');
            const SystemClock = Java.use('android.os.SystemClock');
            const System = Java.use('java.lang.System');

            const makeLocation = function (provider) {
                const loc = Location.$new(provider ? provider : 'gps');
                loc.setLatitude(global.LOCATION_LATITUDE);
                loc.setLongitude(global.LOCATION_LONGITUDE);
                if (typeof global.LOCATION_ACCURACY !== 'undefined') loc.setAccuracy(global.LOCATION_ACCURACY);
                if (typeof global.LOCATION_ALTITUDE !== 'undefined') loc.setAltitude(global.LOCATION_ALTITUDE);
                loc.setTime(System.currentTimeMillis());
                try { loc.setElapsedRealtimeNanos(SystemClock.elapsedRealtimeNanos()); } catch (e) {}
                return loc;
            };

            const LocationManager = Java.use('android.location.LocationManager');
            if (LocationManager.getLastKnownLocation) {
                LocationManager.getLastKnownLocation.overload('java.lang.String').implementation = function (provider) {
                    recon('LocationManager.getLastKnownLocation', provider);
                    return makeLocation(provider);
                };
            }
            console.log('[+] JSAgent hooked LocationManager successfully');

            // Reverse geocoding: hand back an Address with the matching country.
            try {
                const Geocoder = Java.use('android.location.Geocoder');
                const ArrayList = Java.use('java.util.ArrayList');
                const Address = Java.use('android.location.Address');
                const Locale = Java.use('java.util.Locale');
                const ccode = (typeof global.TELECOM_COUNTRY_ISO !== 'undefined') ? global.TELECOM_COUNTRY_ISO.toUpperCase() : '';
                const cname = (typeof global.TELECOM_MCC_COUNTRY_NAME !== 'undefined') ? global.TELECOM_MCC_COUNTRY_NAME : '';
                if (Geocoder.getFromLocation) {
                    Geocoder.getFromLocation.overload('double', 'double', 'int').implementation = function (lat, lon, max) {
                        recon('Geocoder.getFromLocation', lat + ',' + lon);
                        const list = ArrayList.$new();
                        try {
                            const addr = Address.$new(Locale.getDefault());
                            addr.setLatitude(global.LOCATION_LATITUDE);
                            addr.setLongitude(global.LOCATION_LONGITUDE);
                            if (ccode) addr.setCountryCode(ccode);
                            if (cname) addr.setCountryName(cname);
                            list.add(addr);
                        } catch (e) {}
                        return list;
                    };
                }
                console.log('[+] JSAgent hooked Geocoder successfully');
            } catch (e) {
                console.log('[-] Error hooking Geocoder: ' + e);
            }
        } catch (e) {
            console.log('[-] Error hooking location: ' + e);
        }
    }
        },
    };

    // Device-identity spoofs: an evasive app reads these synchronously while
    // deciding whether it's in a sandbox/analysis device and whether to run at
    // all, so they must be live before the first activity's onCreate. The full
    // install is only tens of ms, so everything that answers a device-identity
    // probe goes here — deferring it would let the app see the real device first.
    var CRITICAL = ['build', 'getprop', 'debug_settings', 'telephony', 'ram',
        'display', 'locale_tz', 'sensors', 'package_manager', 'flag_secure',
        'network_vpn', 'proxy', 'battery', 'time', 'keyguard', 'usb',
        'install_referrer', 'accounts', 'wifi', 'location'];

    // Network-time behaviors only: these take effect on the app's first HTTP /
    // TLS traffic, not at startup, and `ssl` is the heaviest block, so they stay
    // off the launch critical path. Drained one-per-main-thread-tick once the UI
    // is up. (The staging mechanism also lets you defer any block here if a
    // specific app ever deadlocks installing it synchronously.)
    var DEFERRED = ['ssl', 'user_agent'];

    installStaged(Java, BLOCKS, CRITICAL, DEFERRED);
}

// Resolve a global native export across Gum API versions (newer builds expose
// findGlobalExportByName/getGlobalExportByName; older ones findExportByName).
function findGlobalSym(name) {
    if (typeof Module.findGlobalExportByName === 'function') {
        return Module.findGlobalExportByName(name);
    }
    if (typeof Module.getGlobalExportByName === 'function') {
        try { return Module.getGlobalExportByName(name); } catch (e) { return null; }
    }
    if (typeof Module.findExportByName === 'function') {
        return Module.findExportByName(null, name);
    }
    return null;
}

// Version-proof readiness check: has the ART VM actually been created yet?
// Queries the JNI invocation API (JNI_GetCreatedJavaVMs) via Gum. This depends
// on no frida-java-bridge internals, and lets us defer the first Java.available
// probe until it is guaranteed to succeed, so getApi() never faults early and
// the bridge never caches a permanent init error (the reason the old code had
// to poke the private _apiError field).
let _jniGetCreatedVMs = null;
function artVmReady() {
    try {
        if (_jniGetCreatedVMs === null) {
            const sym = findGlobalSym('JNI_GetCreatedJavaVMs');
            if (sym === null) return false; // libart not loaded yet
            _jniGetCreatedVMs = new NativeFunction(sym, 'int', ['pointer', 'int', 'pointer']);
        }
        const vmBuf = Memory.alloc(Process.pointerSize);
        const nVms = Memory.alloc(4); // jsize (int)
        if (_jniGetCreatedVMs(vmBuf, 1, nVms) !== 0) return false;
        return nVms.readInt() > 0;
    } catch (e) {
        return false;
    }
}

function waitForJava() {
    let attempts = 0;

    const interval = setInterval(function () {
        attempts++;

        try {
            // Don't touch the bridge until the ART VM exists, see artVmReady().
            if (!artVmReady()) {
                if (attempts % 40 === 0) {
                    console.log('[+] JSAgent: waiting for ART VM... (attempt ' + attempts + ')');
                }
                if (attempts > 6000) {
                    clearInterval(interval);
                    console.error('[!] JSAgent: FATAL - ART VM never came up');
                }
                return;
            }

            // The frida-java-bridge Runtime is provided as the global `bridge`
            // by the prebuilt bundle (frida-tools' bridges/java.js) that the
            // build prepends to this agent — no `require`, no npm.
            const Java = globalThis.__coyote_bridge;
            if (!Java) {
                if (attempts === 1) console.error('[!] JSAgent: bridge global missing');
                return;
            }

            // The VM exists from the zygote fork before the app's ART is fully
            // initialized, so an early getApi() probe can fault (access violation
            // at 0x8). The bridge caches that fault in _apiError and re-throws it
            // forever, so clear it to let a later probe succeed once ART settles.
            // Defensive: tolerate the field being renamed in a future bridge.
            try { if (Java._apiError) Java._apiError = null; } catch (e) {}

            if (Java.available === true) {
                clearInterval(interval);
                console.log('[+] JSAgent: Java available at attempt ' + attempts);
                installGate(Java);
                return;
            }
        } catch (e) {
            if (attempts === 1 || attempts % 40 === 0) {
                console.error('[!] JSAgent: probe error (attempt ' + attempts + '): ' + e.toString());
            }
        }

        if (attempts > 6000) {
            clearInterval(interval);
            console.error('[!] JSAgent: FATAL - Java never initialized after 300 seconds');
        }
    }, 50);
}

function installGate(Java) {
    let hooksInstalled = false;

    // Single install, run at most once no matter which path (eager or fallback)
    // reaches it first.
    const runOnce = function (via) {
        if (hooksInstalled) return;
        hooksInstalled = true;
        console.log('[+] JSAgent: installing hooks (' + (via || 'install') + ')');
        try {
            onJavaReady(Java);
        } catch (e) {
            console.error('[!] JSAgent: onJavaReady error: ' + e);
        }
    };

    // PRIMARY: install the moment Java is available, before the app's main thread
    // reaches its first Activity.onCreate and reads device identity. The old
    // design instead armed a hook on Instrumentation.callActivityOnCreate and
    // installed only when it fired ("right before onCreate"). But arming is the
    // first use of frida-java-bridge — a few hundred ms of class-model + deopt
    // warmup — and on a fast device/app (observed on a physical Pixel 7 /
    // Android 13) the app entered onCreate *before* arming finished, so the hook
    // never fired and NO Java hook ever installed: every Java-level vector leaked
    // the real device value. Installing here is as early as possible and runs
    // with fewer live app threads than at onCreate, so the historical
    // all-thread-suspend deadlock risk the gate was avoiding is no worse
    // (verified on the heavy cocos game Top War: Displayed +1.16s, no ANR).
    try {
        Java.performNow(function () { runOnce('eager'); });
    } catch (e) {
        console.error('[!] JSAgent: eager install faulted (' + e.toString() + '); falling back to waitForApplication');
    }
    if (hooksInstalled) return;

    // FALLBACK: the eager install faulted because this ROM's ART cannot yet
    // deopt from our background thread. Retry on the main-thread timeline — poll
    // until the Application is bound, then install at that point (still before the
    // first onCreate, and past the fragile bootstrap window, so no mid-bootstrap
    // deopt). Routed through runOnce so it can never double-install.
    waitForApplication(Java, runOnce);
}

function waitForApplication(Java, install) {
    let tries = 0;

    const interval = setInterval(function () {
        tries++;

        try { if (Java._apiError) Java._apiError = null; } catch (e) {}

        let ready = false;
        try {
            Java.performNow(function () {
                const ActivityThread = Java.use('android.app.ActivityThread');
                ready = ActivityThread.currentApplication() !== null;
            });
        } catch (e) {
            // Early-ART probes can fault (access violation); keep retrying. If
            // this keeps logging up to the timeout, the fault is persistent.
            if (tries === 1 || tries % 40 === 0) {
                console.error('[!] JSAgent: waitForApplication probe error (try ' + tries + '): ' + e.toString());
            }
        }

        if (ready) {
            clearInterval(interval);
            console.log('[+] JSAgent: Application bound at try ' + tries + '; installing hooks');
            Java.perform(function () {
                install('app-bound');
            });
            return;
        }

        if (tries % 40 === 0) {
            console.log('[+] JSAgent: waiting for Application... (try ' + tries + ')');
        }

        if (tries > 3000) {
            clearInterval(interval);
            console.error('[!] JSAgent: FATAL - Application never bound');
        }
    }, 20);
}

console.log('[+] JSAgent: Waiting for Java to initialize...');
setTimeout(waitForJava, 200);