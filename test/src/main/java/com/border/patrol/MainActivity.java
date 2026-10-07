package com.border.patrol;

import android.app.Activity;
import android.app.ActivityManager;
import android.content.Intent;
import android.graphics.Typeface;
import android.graphics.drawable.GradientDrawable;
import android.net.ConnectivityManager;
import android.net.Network;
import android.net.NetworkCapabilities;
import android.net.NetworkInfo;
import android.net.Proxy;
import android.os.BatteryManager;
import android.os.Build;
import android.os.Bundle;
import android.os.Environment;
import android.os.StatFs;
import android.provider.Settings;
import android.telephony.TelephonyManager;
import android.util.DisplayMetrics;
import android.view.View;
import android.view.WindowManager;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;

import java.io.BufferedReader;
import java.io.InputStreamReader;
import java.lang.reflect.Method;
import java.util.Iterator;
import org.json.JSONObject;

public class MainActivity extends Activity {

    // Modern Color Palette
    private static final int COLOR_BG_PASS = 0xFFECFDF5;      
    private static final int COLOR_TEXT_BG_PASS = 0xFF065F46; 
    private static final int COLOR_BG_FAIL = 0xFFFEF2F2;      
    private static final int COLOR_TEXT_BG_FAIL = 0xFF991B1B; 
    private static final int COLOR_TEXT_PRIMARY = 0xFF0F172A; 
    private static final int COLOR_TEXT_SECONDARY = 0xFF64748B; 
    private static final int COLOR_ACCENT_BG = 0xFFF1F5F9;    

    private int totalChecks = 0;
    private int passedChecks = 0;
    private TextView summaryView;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);

        ScrollView scrollView = new ScrollView(this);
        scrollView.setBackgroundColor(0xFFF8FAFC); 

        LinearLayout container = new LinearLayout(this);
        container.setOrientation(LinearLayout.VERTICAL);
        int padding = dpToPx(16);
        container.setPadding(padding, padding, padding, padding);
        scrollView.addView(container);
        setContentView(scrollView);

        // App Title Header
        TextView titleView = new TextView(this);
        titleView.setText("Border Patrol Validation Report");
        titleView.setTextSize(22);
        titleView.setTypeface(Typeface.create("sans-serif-medium", Typeface.BOLD));
        titleView.setTextColor(COLOR_TEXT_PRIMARY);
        titleView.setPadding(0, dpToPx(8), 0, dpToPx(12));
        container.addView(titleView);

        // Top Summary Banner
        summaryView = new TextView(this);
        summaryView.setTextSize(14);
        summaryView.setTypeface(Typeface.DEFAULT_BOLD);
        summaryView.setPadding(dpToPx(16), dpToPx(14), dpToPx(16), dpToPx(14));
        LinearLayout.LayoutParams summaryParams = new LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT, 
                LinearLayout.LayoutParams.WRAP_CONTENT
        );
        summaryParams.setMargins(0, 0, 0, dpToPx(16));
        summaryView.setLayoutParams(summaryParams);
        container.addView(summaryView);

        try {
            BufferedReader reader = new BufferedReader(new InputStreamReader(getAssets().open("test_profile.json")));
            StringBuilder jsonBuilder = new StringBuilder();
            String line;
            while ((line = reader.readLine()) != null) {
                jsonBuilder.append(line);
            }
            reader.close();

            JSONObject profile = new JSONObject(jsonBuilder.toString());
            JSONObject specs = profile.optJSONObject("specs");
            JSONObject telecom = profile.optJSONObject("telecom");
            JSONObject deviceProfile = profile.optJSONObject("device_profile");
            JSONObject network = profile.optJSONObject("network");
            JSONObject attribution = profile.optJSONObject("attribution");
            JSONObject sysProps = profile.optJSONObject("system_prop");

            // 1. Hardware Specs Section
            if (specs != null) {
                TextView hwHeader = addSectionHeader(container, "Hardware Specs");
                int hwTotal = 0, hwPassed = 0;

                LinearLayout hwPassedContainer = new LinearLayout(this);
                hwPassedContainer.setOrientation(LinearLayout.VERTICAL);
                hwPassedContainer.setVisibility(View.GONE);

                DisplayMetrics metrics = getResources().getDisplayMetrics();
                
                boolean passWidth = metrics.widthPixels == specs.optInt("WIDTH", -1);
                addSpecRow(passWidth ? hwPassedContainer : container, "Display Width", String.valueOf(specs.optInt("WIDTH", -1)), String.valueOf(metrics.widthPixels), passWidth);
                hwTotal++; if (passWidth) hwPassed++;

                boolean passHeight = metrics.heightPixels == specs.optInt("HEIGHT", -1);
                addSpecRow(passHeight ? hwPassedContainer : container, "Display Height", String.valueOf(specs.optInt("HEIGHT", -1)), String.valueOf(metrics.heightPixels), passHeight);
                hwTotal++; if (passHeight) hwPassed++;

                if (specs.has("DENSITY")) {
                    float expectedDensity = parseJsonFloat(specs, "DENSITY", -1f);
                    boolean passDensity = Math.abs(metrics.density - expectedDensity) < 0.01;
                    addSpecRow(passDensity ? hwPassedContainer : container, "Display Density", String.valueOf(expectedDensity), String.valueOf(metrics.density), passDensity);
                    hwTotal++; if (passDensity) hwPassed++;
                }

                if (specs.has("DENSITY_DPI")) {
                    int expectedDpi = specs.getInt("DENSITY_DPI");
                    boolean passDpi = metrics.densityDpi == expectedDpi;
                    addSpecRow(passDpi ? hwPassedContainer : container, "Density DPI", String.valueOf(expectedDpi), String.valueOf(metrics.densityDpi), passDpi);
                    hwTotal++; if (passDpi) hwPassed++;
                }

                if (specs.has("XDPI")) {
                    float expectedXdpi = parseJsonFloat(specs, "XDPI", -1f);
                    boolean passXdpi = Math.abs(metrics.xdpi - expectedXdpi) < 0.01;
                    addSpecRow(passXdpi ? hwPassedContainer : container, "XDPI", String.valueOf(expectedXdpi), String.valueOf(metrics.xdpi), passXdpi);
                    hwTotal++; if (passXdpi) hwPassed++;
                }

                StatFs stat = new StatFs(Environment.getDataDirectory().getPath());
                long totalBytes = stat.getTotalBytes();
                long freeBytes = stat.getAvailableBytes();

                if (specs.has("TOTAL_SPACE")) {
                    long expTotal = specs.getLong("TOTAL_SPACE");
                    boolean passTotal = Math.abs(totalBytes - expTotal) < (1024L * 1024L * 10L);
                    addSpecRow(passTotal ? hwPassedContainer : container, "Total Storage", String.valueOf(expTotal), String.valueOf(totalBytes), passTotal);
                    hwTotal++; if (passTotal) hwPassed++;
                }
                if (specs.has("FREE_SPACE")) {
                    long expFree = specs.getLong("FREE_SPACE");
                    boolean passFree = Math.abs(freeBytes - expFree) < (expFree * 0.25);
                    addSpecRow(passFree ? hwPassedContainer : container, "Free Storage", String.valueOf(expFree), String.valueOf(freeBytes), passFree);
                    hwTotal++; if (passFree) hwPassed++;
                }

                ActivityManager am = (ActivityManager) getSystemService(ACTIVITY_SERVICE);
                ActivityManager.MemoryInfo memInfo = new ActivityManager.MemoryInfo();
                if (am != null) {
                    am.getMemoryInfo(memInfo);
                    if (specs.has("TOTAL_RAM")) {
                        long expRam = specs.getLong("TOTAL_RAM");
                        boolean passRam = Math.abs(memInfo.totalMem - expRam) < (1024L * 1024L * 50L);
                        addSpecRow(passRam ? hwPassedContainer : container, "Total RAM", String.valueOf(expRam), String.valueOf(memInfo.totalMem), passRam);
                        hwTotal++; if (passRam) hwPassed++;
                    }
                    if (specs.has("AVAIL_RAM")) {
                        long expAvailRam = specs.getLong("AVAIL_RAM");
                        double variance = expAvailRam * 0.05;
                        boolean passAvail = Math.abs(memInfo.availMem - expAvailRam) <= variance;
                        addSpecRow(passAvail ? hwPassedContainer : container, "Avail RAM (~5%)", String.valueOf(expAvailRam), String.valueOf(memInfo.availMem), passAvail);
                        hwTotal++; if (passAvail) hwPassed++;
                    }
                }

                hwHeader.setText("Hardware Specs  •  " + hwPassed + " / " + hwTotal + " Passed");
                addPassedDropdown(container, hwPassedContainer, hwPassed);
            }

            // 2. Telephony Group
            if (telecom != null) {
                TextView telHeader = addSectionHeader(container, "Vector: Telephony");
                int telTotal = 0, telPassed = 0;
                LinearLayout telPassedContainer = new LinearLayout(this);
                telPassedContainer.setOrientation(LinearLayout.VERTICAL);
                telPassedContainer.setVisibility(View.GONE);

                TelephonyManager tm = (TelephonyManager) getSystemService(TELEPHONY_SERVICE);
                if (tm != null) {
                    if (telecom.has("operator_name")) {
                        String expOp = telecom.getString("operator_name");
                        String actOp = tm.getSimOperatorName();
                        boolean passOp = expOp.equals(actOp);
                        addPropRow(passOp ? telPassedContainer : container, "Sim Operator Name", expOp, actOp, passOp);
                        telTotal++; if (passOp) telPassed++;
                    }
                    if (telecom.has("mcc") && telecom.has("mnc")) {
                        String expMccMnc = telecom.getString("mcc") + telecom.getString("mnc");
                        String actMccMnc = tm.getSimOperator();
                        boolean passMcc = expMccMnc.equals(actMccMnc);
                        addPropRow(passMcc ? telPassedContainer : container, "MCC+MNC", expMccMnc, actMccMnc, passMcc);
                        telTotal++; if (passMcc) telPassed++;
                    }
                    if (telecom.has("country_iso")) {
                        String expIso = telecom.getString("country_iso");
                        String actIso = tm.getSimCountryIso();
                        boolean passIso = expIso.equalsIgnoreCase(actIso);
                        addPropRow(passIso ? telPassedContainer : container, "Sim Country ISO", expIso, actIso, passIso);
                        telTotal++; if (passIso) telPassed++;
                    }
                    if (telecom.has("sim_state_num")) {
                        int expState = telecom.getInt("sim_state_num");
                        int actState = tm.getSimState();
                        boolean passState = (expState == actState);
                        addPropRow(passState ? telPassedContainer : container, "SIM State Num", String.valueOf(expState), String.valueOf(actState), passState);
                        telTotal++; if (passState) telPassed++;
                    }
                }

                telHeader.setText("Vector: Telephony  •  " + telPassed + " / " + telTotal + " Passed");
                addPassedDropdown(container, telPassedContainer, telPassed);
            }

            // 3. Environment API Group (Battery, Package, User-Agent)
            {
                TextView envHeader = addSectionHeader(container, "Vector: Env / APIs");
                int envTotal = 0, envPassed = 0;
                LinearLayout envPassedContainer = new LinearLayout(this);
                envPassedContainer.setOrientation(LinearLayout.VERTICAL);
                envPassedContainer.setVisibility(View.GONE);

                // Battery
                BatteryManager bm = (BatteryManager) getSystemService(BATTERY_SERVICE);
                if (bm != null && specs != null && specs.has("BATTERY_LEVEL")) {
                    int expBatt = specs.getInt("BATTERY_LEVEL");
                    int actBatt = bm.getIntProperty(BatteryManager.BATTERY_PROPERTY_CAPACITY);
                    boolean passBatt = expBatt == actBatt;
                    addPropRow(passBatt ? envPassedContainer : container, "Battery Level", String.valueOf(expBatt), String.valueOf(actBatt), passBatt);
                    envTotal++; if (passBatt) envPassed++;
                }

                // Package Manager Installer Bypass
                try {
                    String expInstaller = (attribution != null && attribution.has("install_package")) ? attribution.getString("install_package") : "com.android.vending";
                    String actInstaller = getPackageManager().getInstallerPackageName(getPackageName());
                    boolean passInstaller = expInstaller.equals(actInstaller);
                    addPropRow(passInstaller ? envPassedContainer : container, "Installer Package", expInstaller, actInstaller != null ? actInstaller : "null", passInstaller);
                    envTotal++; if (passInstaller) envPassed++;
                } catch (Exception e) {}

                // Global Settings ADB Spoof
                try {
                    int adbVal = Settings.Global.getInt(getContentResolver(), Settings.Global.ADB_ENABLED, -1);
                    boolean passAdb = adbVal == 0;
                    addPropRow(passAdb ? envPassedContainer : container, "Global ADB_ENABLED", "0", String.valueOf(adbVal), passAdb);
                    envTotal++; if (passAdb) envPassed++;
                } catch (Exception e) {}

                // User Agent property
                String httpAgent = System.getProperty("http.agent");
                boolean passUa = httpAgent != null && httpAgent.contains("Dalvik/2.1.0");
                addPropRow(passUa ? envPassedContainer : container, "http.agent Prop", "Contains 'Dalvik/2.1.0'", httpAgent != null ? httpAgent : "null", passUa);
                envTotal++; if (passUa) envPassed++;

                envHeader.setText("Vector: Env / APIs  •  " + envPassed + " / " + envTotal + " Passed");
                addPassedDropdown(container, envPassedContainer, envPassed);
            }

            // 4. Install Referrer Vector
            if (attribution != null && attribution.has("install_referrer")) {
                TextView refHeader = addSectionHeader(container, "Vector: Install Referrer");
                int refTotal = 0, refPassed = 0;
                LinearLayout refPassedContainer = new LinearLayout(this);
                refPassedContainer.setOrientation(LinearLayout.VERTICAL);
                refPassedContainer.setVisibility(View.GONE);

                String expectedReferrer = attribution.getString("install_referrer");

                // Test via Bundle getString hook override
                Bundle bundle = new Bundle();
                bundle.putString("install_referrer", "unhooked_dummy_val");
                String actualReferrer = bundle.getString("install_referrer");

                boolean passReferrer = expectedReferrer.equals(actualReferrer);
                addPropRow(passReferrer ? refPassedContainer : container, "Bundle install_referrer", expectedReferrer, actualReferrer, passReferrer);
                refTotal++; if (passReferrer) refPassed++;

                refHeader.setText("Vector: Install Referrer  •  " + refPassed + " / " + refTotal + " Passed");
                addPassedDropdown(container, refPassedContainer, refPassed);
            }

            // 5. USB Action Bypass Vector
            {
                TextView usbHeader = addSectionHeader(container, "Vector: USB Action Bypass");
                int usbTotal = 0, usbPassed = 0;
                LinearLayout usbPassedContainer = new LinearLayout(this);
                usbPassedContainer.setOrientation(LinearLayout.VERTICAL);
                usbPassedContainer.setVisibility(View.GONE);

                Intent usbIntent = new Intent("android.hardware.usb.action.USB_STATE");
                usbIntent.putExtra("connected", true);
                usbIntent.putExtra("adb", true);
                usbIntent.putExtra("configured", true);

                // Test Intent.getBooleanExtra hook override
                boolean actConnected = usbIntent.getBooleanExtra("connected", true);
                boolean passConnected = !actConnected; // Expect false
                addPropRow(passConnected ? usbPassedContainer : container, "USB Extra 'connected'", "false", String.valueOf(actConnected), passConnected);
                usbTotal++; if (passConnected) usbPassed++;

                boolean actAdb = usbIntent.getBooleanExtra("adb", true);
                boolean passAdb = !actAdb; // Expect false
                addPropRow(passAdb ? usbPassedContainer : container, "USB Extra 'adb'", "false", String.valueOf(actAdb), passAdb);
                usbTotal++; if (passAdb) usbPassed++;

                // Test Intent.getExtras hook override
                Bundle extras = usbIntent.getExtras();
                boolean bundleConnected = extras != null && extras.getBoolean("connected", true);
                boolean passBundle = !bundleConnected;
                addPropRow(passBundle ? usbPassedContainer : container, "USB Bundle 'connected'", "false", String.valueOf(bundleConnected), passBundle);
                usbTotal++; if (passBundle) usbPassed++;

                usbHeader.setText("Vector: USB Action Bypass  •  " + usbPassed + " / " + usbTotal + " Passed");
                addPassedDropdown(container, usbPassedContainer, usbPassed);
            }

            // 6. FLAG_SECURE Bypass Vector
            {
                TextView flagHeader = addSectionHeader(container, "Vector: FLAG_SECURE Bypass");
                int flagTotal = 0, flagPassed = 0;
                LinearLayout flagPassedContainer = new LinearLayout(this);
                flagPassedContainer.setOrientation(LinearLayout.VERTICAL);
                flagPassedContainer.setVisibility(View.GONE);

                // Attempt to apply FLAG_SECURE to current Window
                getWindow().addFlags(WindowManager.LayoutParams.FLAG_SECURE);
                int currentFlags = getWindow().getAttributes().flags;
                boolean hasSecureFlag = (currentFlags & WindowManager.LayoutParams.FLAG_SECURE) != 0;
                boolean passSecure = !hasSecureFlag; // Expect flag to be stripped by hook

                addPropRow(passSecure ? flagPassedContainer : container, "FLAG_SECURE Stripped", "false (Removed)", String.valueOf(hasSecureFlag), passSecure);
                flagTotal++; if (passSecure) flagPassed++;

                flagHeader.setText("Vector: FLAG_SECURE Bypass  •  " + flagPassed + " / " + flagTotal + " Passed");
                addPassedDropdown(container, flagPassedContainer, flagPassed);
            }

            // 7. Network, VPN & Proxy Vector
            {
                TextView netHeader = addSectionHeader(container, "Vector: Network, VPN & Proxy");
                int netTotal = 0, netPassed = 0;
                LinearLayout netPassedContainer = new LinearLayout(this);
                netPassedContainer.setOrientation(LinearLayout.VERTICAL);
                netPassedContainer.setVisibility(View.GONE);

                // Test 1: Proxy Host Cloaking
                String proxyHost = Proxy.getDefaultHost();
                boolean passProxy = (proxyHost == null || proxyHost.isEmpty());
                addPropRow(passProxy ? netPassedContainer : container, "Proxy Default Host", "null / empty", proxyHost != null ? proxyHost : "null", passProxy);
                netTotal++; if (passProxy) netPassed++;

                // Test 2: VPN Transport Cloaking
                ConnectivityManager cm = (ConnectivityManager) getSystemService(CONNECTIVITY_SERVICE);
                boolean vpnActive = false;
                if (cm != null) {
                    if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.M) {
                        Network activeNetwork = cm.getActiveNetwork();
                        if (activeNetwork != null) {
                            NetworkCapabilities caps = cm.getNetworkCapabilities(activeNetwork);
                            if (caps != null) {
                                vpnActive = caps.hasTransport(NetworkCapabilities.TRANSPORT_VPN);
                            }
                        }
                    } else {
                        NetworkInfo netInfo = cm.getNetworkInfo(ConnectivityManager.TYPE_VPN);
                        if (netInfo != null) {
                            vpnActive = netInfo.isConnected();
                        }
                    }
                }
                boolean passVpn = !vpnActive; // Expect false
                addPropRow(passVpn ? netPassedContainer : container, "VPN Transport Active", "false", String.valueOf(vpnActive), passVpn);
                netTotal++; if (passVpn) netPassed++;

                // Test 3: Network Type Spoofing
                if (network != null && network.has("net_type")) {
                    int expNetType = network.getInt("net_type");
                    NetworkInfo activeInfo = cm != null ? cm.getActiveNetworkInfo() : null;
                    int actNetType = activeInfo != null ? activeInfo.getType() : -1;
                    boolean passNetType = (expNetType == actNetType);
                    addPropRow(passNetType ? netPassedContainer : container, "Active Network Type", String.valueOf(expNetType), String.valueOf(actNetType), passNetType);
                    netTotal++; if (passNetType) netPassed++;
                }

                netHeader.setText("Vector: Network, VPN & Proxy  •  " + netPassed + " / " + netTotal + " Passed");
                addPassedDropdown(container, netPassedContainer, netPassed);
            }

            // 8. System Properties & Device Profile Grouped by Vector
            if (sysProps != null && sysProps.length() > 0) {
                // Vector 1: Reflection
                TextView refHeader = addSectionHeader(container, "Vector: Reflection API");
                int refTotal = 0, refPassed = 0;
                LinearLayout refPassedContainer = new LinearLayout(this);
                refPassedContainer.setOrientation(LinearLayout.VERTICAL);
                refPassedContainer.setVisibility(View.GONE);

                Class<?> spClass = Class.forName("android.os.SystemProperties");
                Method getMethod = spClass.getMethod("get", String.class, String.class);

                Iterator<String> keysReflection = sysProps.keys();
                while (keysReflection.hasNext()) {
                    String propKey = keysReflection.next();
                    String expectedVal = sysProps.getString(propKey);
                    String valReflection = (String) getMethod.invoke(null, propKey, "");
                    boolean passReflection = valReflection.equals(expectedVal);
                    addPropRow(passReflection ? refPassedContainer : container, propKey, expectedVal, valReflection, passReflection);
                    refTotal++; if (passReflection) refPassed++;
                }
                refHeader.setText("Vector: Reflection API  •  " + refPassed + " / " + refTotal + " Passed");
                addPassedDropdown(container, refPassedContainer, refPassed);

                // Vector 2: Shell Runtime Exec
                TextView shellHeader = addSectionHeader(container, "Vector: Shell Runtime Execution");
                int shellTotal = 0, shellPassed = 0;
                LinearLayout shellPassedContainer = new LinearLayout(this);
                shellPassedContainer.setOrientation(LinearLayout.VERTICAL);
                shellPassedContainer.setVisibility(View.GONE);

                Iterator<String> keysShell = sysProps.keys();
                while (keysShell.hasNext()) {
                    String propKey = keysShell.next();
                    String expectedVal = sysProps.getString(propKey);
                    String valShell = executeShellGetProp(propKey);
                    boolean passShell = valShell.equals(expectedVal);
                    addPropRow(passShell ? shellPassedContainer : container, propKey, expectedVal, valShell, passShell);
                    shellTotal++; if (passShell) shellPassed++;
                }
                shellHeader.setText("Vector: Shell Runtime  •  " + shellPassed + " / " + shellTotal + " Passed");
                addPassedDropdown(container, shellPassedContainer, shellPassed);

                // Vector 3: Shell Command Execution (Non-getprop Baseline)
                TextView shellBaselineHeader = addSectionHeader(container, "Vector: Shell Command Baseline");
                int shellBaselineTotal = 0, shellBaselinePassed = 0;
                LinearLayout shellBaselinePassedContainer = new LinearLayout(this);
                shellBaselinePassedContainer.setOrientation(LinearLayout.VERTICAL);
                shellBaselinePassedContainer.setVisibility(View.GONE);

                // Test 1: Echo command
                String echoResult = executeShellCommand("echo Hello");
                boolean passEcho = echoResult.equals("Hello");
                addPropRow(passEcho ? shellBaselinePassedContainer : container, "echo Hello", "Hello", echoResult, passEcho);
                shellBaselineTotal++; if (passEcho) shellBaselinePassed++;

                // Test 2: Additional baseline test (e.g., whoami)
                String whoamiResult = executeShellCommand("whoami");
                boolean passWhoami = !whoamiResult.isEmpty() && !whoamiResult.contains("[Error");
                addPropRow(passWhoami ? shellBaselinePassedContainer : container, "whoami", "(non-empty)", whoamiResult, passWhoami);
                shellBaselineTotal++; if (passWhoami) shellBaselinePassed++;

                shellBaselineHeader.setText("Vector: Shell Baseline  •  " + shellBaselinePassed + " / " + shellBaselineTotal + " Passed");
                addPassedDropdown(container, shellBaselinePassedContainer, shellBaselinePassed);

                // Vector 4: Build Class Mapping
                TextView buildHeader = addSectionHeader(container, "Vector: android.os.Build Mapping");
                int buildTotal = 0, buildPassed = 0;
                LinearLayout buildPassedContainer = new LinearLayout(this);
                buildPassedContainer.setOrientation(LinearLayout.VERTICAL);
                buildPassedContainer.setVisibility(View.GONE);

                if (deviceProfile != null) {
                    checkBuildField(buildPassedContainer, container, "Build.MODEL", deviceProfile.optString("model"), Build.MODEL, buildTotal++, buildPassed);
                    if (deviceProfile.optString("model").equals(Build.MODEL)) buildPassed++;

                    checkBuildField(buildPassedContainer, container, "Build.MANUFACTURER", deviceProfile.optString("manufacturer"), Build.MANUFACTURER, buildTotal++, buildPassed);
                    if (deviceProfile.optString("manufacturer").equals(Build.MANUFACTURER)) buildPassed++;

                    checkBuildField(buildPassedContainer, container, "Build.BRAND", deviceProfile.optString("brand"), Build.BRAND, buildTotal++, buildPassed);
                    if (deviceProfile.optString("brand").equals(Build.BRAND)) buildPassed++;

                    checkBuildField(buildPassedContainer, container, "Build.DEVICE", deviceProfile.optString("device"), Build.DEVICE, buildTotal++, buildPassed);
                    if (deviceProfile.optString("device").equals(Build.DEVICE)) buildPassed++;

                    checkBuildField(buildPassedContainer, container, "Build.BOARD", deviceProfile.optString("board"), Build.BOARD, buildTotal++, buildPassed);
                    if (deviceProfile.optString("board").equals(Build.BOARD)) buildPassed++;

                    checkBuildField(buildPassedContainer, container, "Build.HARDWARE", deviceProfile.optString("hardware"), Build.HARDWARE, buildTotal++, buildPassed);
                    if (deviceProfile.optString("hardware").equals(Build.HARDWARE)) buildPassed++;

                    checkBuildField(buildPassedContainer, container, "Build.FINGERPRINT", deviceProfile.optString("fingerprint"), Build.FINGERPRINT, buildTotal++, buildPassed);
                    if (deviceProfile.optString("fingerprint").equals(Build.FINGERPRINT)) buildPassed++;

                    checkBuildField(buildPassedContainer, container, "Build.SERIAL", deviceProfile.optString("serial"), Build.SERIAL, buildTotal++, buildPassed);
                    if (deviceProfile.optString("serial").equals(Build.SERIAL)) buildPassed++;
                }

                buildHeader.setText("Vector: Build Mapping  •  " + buildPassed + " / " + buildTotal + " Passed");
                addPassedDropdown(container, buildPassedContainer, buildPassed);
            }

            updateSummaryBanner();

        } catch (Exception e) {
            summaryView.setText("Error generating validation report: " + e.getMessage());
            summaryView.setTextColor(COLOR_TEXT_BG_FAIL);
            summaryView.setBackground(createRoundedDrawable(COLOR_BG_FAIL, 8));
        }
    }

    private float parseJsonFloat(JSONObject obj, String key, float defaultVal) {
        if (obj == null || !obj.has(key)) return defaultVal;
        try {
            String valStr = obj.getString(key).replace("f", "").trim();
            return Float.parseFloat(valStr);
        } catch (Exception e) {
            return (float) obj.optDouble(key, defaultVal);
        }
    }

    private void checkBuildField(LinearLayout passedContainer, LinearLayout mainContainer, String label, String expected, String actual, int totalCount, int passedCount) {
        boolean pass = expected.equals(actual);
        addPropRow(pass ? passedContainer : mainContainer, label, expected, actual, pass);
    }

    private void addPassedDropdown(LinearLayout mainContainer, LinearLayout passedContainer, int passedCount) {
        if (passedCount == 0) return;

        TextView toggleButton = new TextView(this);
        toggleButton.setText("▶  Show " + passedCount + " Passed Check(s)");
        toggleButton.setTextSize(13);
        toggleButton.setTypeface(Typeface.DEFAULT_BOLD);
        toggleButton.setTextColor(COLOR_TEXT_SECONDARY);
        toggleButton.setBackground(createRoundedDrawable(COLOR_ACCENT_BG, 8));
        toggleButton.setPadding(dpToPx(14), dpToPx(10), dpToPx(14), dpToPx(10));

        LinearLayout.LayoutParams params = new LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT, 
                LinearLayout.LayoutParams.WRAP_CONTENT
        );
        params.setMargins(0, dpToPx(4), 0, dpToPx(12));
        toggleButton.setLayoutParams(params);

        toggleButton.setOnClickListener(v -> {
            if (passedContainer.getVisibility() == View.GONE) {
                passedContainer.setVisibility(View.VISIBLE);
                toggleButton.setText("▼  Hide " + passedCount + " Passed Check(s)");
            } else {
                passedContainer.setVisibility(View.GONE);
                toggleButton.setText("▶  Show " + passedCount + " Passed Check(s)");
            }
        });

        mainContainer.addView(toggleButton);
        mainContainer.addView(passedContainer);
    }

    private void updateSummaryBanner() {
        boolean allPassed = (passedChecks == totalChecks);
        summaryView.setText(allPassed ? "✓ All " + totalChecks + " system checks passed successfully!" : "⚠ " + passedChecks + " of " + totalChecks + " checks passed");
        summaryView.setTextColor(allPassed ? COLOR_TEXT_BG_PASS : COLOR_TEXT_BG_FAIL);
        summaryView.setBackground(createRoundedDrawable(allPassed ? COLOR_BG_PASS : COLOR_BG_FAIL, 8));
    }

    private String executeShellGetProp(String propKey) {
        try {
            Process process = Runtime.getRuntime().exec("getprop " + propKey);
            BufferedReader reader = new BufferedReader(new InputStreamReader(process.getInputStream()));
            String line = reader.readLine();
            reader.close();
            return line != null ? line.trim() : "";
        } catch (Exception e) {
            return "[Error executing shell]";
        }
    }

    private String executeShellCommand(String command) {
        try {
            Process process = Runtime.getRuntime().exec(command);
            BufferedReader reader = new BufferedReader(new InputStreamReader(process.getInputStream()));
            String line = reader.readLine();
            reader.close();
            return line != null ? line.trim() : "";
        } catch (Exception e) {
            return "[Error executing shell: " + e.getMessage() + "]";
        }
    }

    private TextView addSectionHeader(LinearLayout container, String text) {
        TextView tv = new TextView(this);
        tv.setText(text);
        tv.setTextSize(15);
        tv.setTypeface(Typeface.create("sans-serif-medium", Typeface.BOLD));
        tv.setTextColor(COLOR_TEXT_PRIMARY);
        tv.setPadding(0, dpToPx(16), 0, dpToPx(8));
        container.addView(tv);
        return tv;
    }

    private void addSpecRow(LinearLayout targetContainer, String name, String expected, String actual, boolean pass) {
        totalChecks++;
        if (pass) passedChecks++;
        createCardRow(targetContainer, name, expected, actual, pass);
    }

    private void addPropRow(LinearLayout targetContainer, String label, String expected, String actual, boolean pass) {
        totalChecks++;
        if (pass) passedChecks++;
        createCardRow(targetContainer, label, expected, actual, pass);
    }

    private void createCardRow(LinearLayout targetContainer, String label, String expected, String actual, boolean pass) {
        TextView tv = new TextView(this);
        
        String badge = pass ? "✓ PASS" : "✕ FAIL";
        String content = badge + "  " + label + "\n   Exp: " + expected + "\n   Act: " + actual;

        tv.setText(content);
        tv.setTextSize(12);
        tv.setTypeface(Typeface.MONOSPACE);
        tv.setPadding(dpToPx(14), dpToPx(10), dpToPx(14), dpToPx(10));
        tv.setTextColor(pass ? COLOR_TEXT_BG_PASS : COLOR_TEXT_BG_FAIL);
        tv.setBackground(createRoundedDrawable(pass ? COLOR_BG_PASS : COLOR_BG_FAIL, 6));

        LinearLayout.LayoutParams params = new LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT, 
                LinearLayout.LayoutParams.WRAP_CONTENT
        );
        params.setMargins(0, dpToPx(3), 0, dpToPx(3));
        tv.setLayoutParams(params);
        targetContainer.addView(tv);
    }

    private GradientDrawable createRoundedDrawable(int backgroundColor, float cornerRadiusDp) {
        GradientDrawable drawable = new GradientDrawable();
        drawable.setShape(GradientDrawable.RECTANGLE);
        drawable.setColor(backgroundColor);
        drawable.setCornerRadius(dpToPx((int) cornerRadiusDp));
        return drawable;
    }

    private int dpToPx(int dp) {
        return Math.round(dp * getResources().getDisplayMetrics().density);
    }
}