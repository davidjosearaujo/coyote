if (typeof globalThis.COYOTE_DEBUG === 'undefined') {
    console.log = function () {};
    console.error = function () {};
    console.warn = function () {};
}

console.log('[+] JSAgent: Starting...');

function onJavaReady(Java) {
    console.log('[+] JSAgent: Java is ready! Attaching Java hooks...');

    // ============================================
    // BLOCK 2: Jitter Utility Function
    // ============================================
    function getRealisticJitter(baseValue, variancePercent) {
        const variance = baseValue * variancePercent;
        const offset = (Math.random() * variance * 2) - variance;
        return Math.floor(baseValue + offset);
    }

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

            Intent.getIntExtra.overloads.forEach(function(overload) {
                overload.implementation = function() {
                    const ret = overload.apply(this, arguments);
                    const key = arguments[0];
                    const map = {
                        'plugged': 0,
                        'status': 3,
                        'level': battery_level,
                        'scale': ret === 0 ? 100 : ret,
                        'health': 2,
                        'voltage': ret === 0 ? spoof_voltage : ret,
                        'temperature': ret === 0 ? spoof_temp : ret
                    };
                    return map[key] !== undefined ? map[key] : ret;
                };
            });

            Bundle.getInt.overloads.forEach(function(overload) {
                overload.implementation = function() {
                    const ret = overload.apply(this, arguments);
                    const key = arguments[0];
                    const map = {
                        'plugged': 0,
                        'status': 3,
                        'level': battery_level,
                        'scale': ret === 0 ? 100 : ret,
                        'health': 2,
                        'voltage': ret === 0 ? spoof_voltage : ret,
                        'temperature': ret === 0 ? spoof_temp : ret
                    };
                    return map[key] !== undefined ? map[key] : ret;
                };
            });

            console.log('[+] JSAgent hooked battery info successfully');
        } catch (e) {
            console.log('[-] Error hooking battery info: ' + e);
        }
    }

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

            Bundle.getString.overloads.forEach(function(overload) {
                overload.implementation = function() {
                    const key = arguments[0];
                    const ret = overload.apply(this, arguments);
                    return key === 'install_referrer' ? installReferrer : ret;
                };
            });

            Bundle.getLong.overloads.forEach(function(overload) {
                overload.implementation = function() {
                    const key = arguments[0];
                    const ret = overload.apply(this, arguments);

                    const timeMap = {
                        'install_begin_timestamp_seconds': cachedInstallTime,
                        'install_begin_timestamp_server_seconds': cachedInstallTime,
                        'referrer_click_timestamp_seconds': cachedClickTime,
                        'referrer_click_timestamp_server_seconds': cachedClickTime
                    };

                    return timeMap[key] !== undefined ? timeMap[key] : ret;
                };
            });

            console.log('[+] JSAgent hooked install referrer successfully');
        } catch (e) {
            console.log('[-] Error hooking install referrer: ' + e);
        }
    }

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

            const randomStr = function(len) {
                let out = '';
                for (let i = 0; i < len; i++) {
                    out += Math.floor(Math.random() * 10).toString();
                }
                return out;
            };

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

    // ============================================
    // BLOCK 10: Development & Debug Settings Bypass
    // ============================================
    try {
        const genHex = function(len) {
            let hex = '';
            for (let i = 0; i < len; i++) {
                hex += Math.floor(Math.random() * 16).toString(16);
            }
            return hex;
        };

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
            const allowedKeys = classConfigs[className];

            if (clazz.getString) {
                clazz.getString.overloads.forEach(function(overload) {
                    overload.implementation = function() {
                        const name = arguments[1];
                        if (allowedKeys.indexOf(name) !== -1 && settingsMap[name] !== undefined) {
                            return settingsMap[name];
                        }
                        return this.getString.apply(this, arguments);
                    };
                });
            }

            if (clazz.getInt) {
                clazz.getInt.overloads.forEach(function(overload) {
                    overload.implementation = function() {
                        const name = arguments[1];
                        if (allowedKeys.indexOf(name) !== -1 && settingsMap[name] !== undefined) {
                            return parseInt(settingsMap[name], 10);
                        }
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

    // ============================================
    // BLOCK 12: Network Info Spoofing Hooks
    // ============================================
    if (typeof global.NETWORK_NET_TYPE !== 'undefined') {
        try {
            const NetworkInfo = Java.use('android.net.NetworkInfo');

            const netType = global.NETWORK_NET_TYPE;
            const typeName = (netType === 1) ? 'WIFI' : 'MOBILE';
            const subType = (netType === 1) ? '' : 'LTE';
            const fakeNetInfo = NetworkInfo.$new(netType, 0, typeName, subType);

            console.log('[+] JSAgent hooked network info successfully');
        } catch (e) {
            console.log('[-] Error hooking network info: ' + e);
        }
    }

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

            const System = Java.use('java.lang.System');
            for (let i = 0; i < System.getProperty.overloads.length; i++) {
                System.getProperty.overloads[i].implementation = function () {
                    const key = arguments[0];
                    if (key === 'http.agent') {
                        return nativePrefix + UA;
                    }
                    return System.getProperty.apply(this, arguments);
                };
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
            'com.privateinternetaccess.android'
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
            const chars = 'ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_';
            let result = '';
            for (let i = 0; i < length; i++) {
                result += chars.charAt(Math.floor(Math.random() * chars.length));
            }
            return result;
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

    console.log('[+] JSAgent: All hook blocks installed');
}

function waitForJava() {
    let attempts = 0;

    const interval = setInterval(function () {
        attempts++;

        try {
            const mod = require("frida-java-bridge");
            const Java = (mod && mod.default) ? mod.default : mod;

            if (Java._apiError) {
                Java._apiError = null;
            }

            if (Java && typeof Java.available !== 'undefined' && Java.available === true) {
                clearInterval(interval);
                console.log('[+] JSAgent: Java.available === true at attempt ' + attempts);

                installGate(Java);
                return;
            }

            if (attempts % 40 === 0) {
                console.log('[+] JSAgent: Attempt ' + attempts + ' - Java.available: ' + Java.available);
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

    const runOnce = function () {
        if (hooksInstalled) return;
        hooksInstalled = true;
        console.log('[+] JSAgent: gate fired on main thread; installing hooks before onCreate');
        try {
            onJavaReady(Java);
        } catch (e) {
            console.error('[!] JSAgent: onJavaReady error: ' + e);
        }
    };

    try {
        Java.performNow(function () {
            const Instrumentation = Java.use('android.app.Instrumentation');
            const overloads = Instrumentation.callActivityOnCreate.overloads;
            overloads.forEach(function (ov) {
                ov.implementation = function () {
                    runOnce();
                    return ov.apply(this, arguments);
                };
            });
            console.log('[+] JSAgent: gate armed on Instrumentation.callActivityOnCreate (' + overloads.length + ' overload(s))');
        });
    } catch (e) {
        console.error('[!] JSAgent: failed to arm gate (' + e.toString() + '); falling back to waitForApplication');
        waitForApplication(Java);
    }
}

function waitForApplication(Java) {
    let tries = 0;

    const interval = setInterval(function () {
        tries++;

        let ready = false;
        try {
            Java.performNow(function () {
                const ActivityThread = Java.use('android.app.ActivityThread');
                ready = ActivityThread.currentApplication() !== null;
            });
        } catch (e) {
            if (tries === 1) {
                console.error('[!] JSAgent: waitForApplication error: ' + e.toString());
            }
        }

        if (ready) {
            clearInterval(interval);
            console.log('[+] JSAgent: Application bound at try ' + tries + '; installing hooks');
            Java.perform(function () {
                onJavaReady(Java);
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