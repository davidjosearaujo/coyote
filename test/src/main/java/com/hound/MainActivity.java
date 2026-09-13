package com.hound;

import android.app.Activity;
import android.app.ActivityManager;
import android.os.Build;
import android.os.Bundle;
import android.os.Environment;
import android.os.StatFs;
import android.util.DisplayMetrics;
import android.view.Gravity;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;

import java.util.Iterator;
import org.json.JSONObject;

import java.io.BufferedReader;
import java.io.InputStreamReader;
import java.lang.reflect.Method;
import java.util.HashMap;
import java.util.Map;

public class MainActivity extends Activity {

    private int totalChecks = 0;
    private int passedChecks = 0;
    private TextView summaryView;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);

        ScrollView scrollView = new ScrollView(this);
        LinearLayout container = new LinearLayout(this);
        container.setOrientation(LinearLayout.VERTICAL);
        container.setPadding(24, 24, 24, 24);
        scrollView.addView(container);
        setContentView(scrollView);

        addHeader(container, "=== HOUND SYSTEM VALIDATION REPORT ===");

        // Summary Banner placed right at the top
        summaryView = new TextView(this);
        summaryView.setTextSize(14);
        summaryView.setTypeface(null, android.graphics.Typeface.BOLD);
        summaryView.setPadding(16, 16, 16, 16);
        LinearLayout.LayoutParams summaryParams = new LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT, 
                LinearLayout.LayoutParams.WRAP_CONTENT
        );
        summaryParams.setMargins(0, 0, 0, 16);
        summaryView.setLayoutParams(summaryParams);
        container.addView(summaryView);

        try {
            // Load test profile from assets
            BufferedReader reader = new BufferedReader(new InputStreamReader(getAssets().open("test_profile.json")));
            StringBuilder jsonBuilder = new StringBuilder();
            String line;
            while ((line = reader.readLine()) != null) {
                jsonBuilder.append(line);
            }
            reader.close();

            JSONObject profile = new JSONObject(jsonBuilder.toString());
            JSONObject specs = profile.optJSONObject("specs");
            JSONObject sysProps = profile.optJSONObject("system_prop");

            // 1. Hardware Specs Validation
            if (specs != null) {
                addSectionHeader(container, "--- HARDWARE SPECS ---");

                DisplayMetrics metrics = getResources().getDisplayMetrics();
                addSpecRow(container, "Display Width", String.valueOf(specs.optInt("WIDTH", -1)), String.valueOf(metrics.widthPixels), metrics.widthPixels == specs.optInt("WIDTH", -1));
                addSpecRow(container, "Display Height", String.valueOf(specs.optInt("HEIGHT", -1)), String.valueOf(metrics.heightPixels), metrics.heightPixels == specs.optInt("HEIGHT", -1));

                StatFs stat = new StatFs(Environment.getDataDirectory().getPath());
                long totalBytes = stat.getTotalBytes();
                long freeBytes = stat.getAvailableBytes();

                if (specs.has("TOTAL_SPACE")) {
                    long expTotal = specs.getLong("TOTAL_SPACE");
                    boolean passTotal = Math.abs(totalBytes - expTotal) < (1024L * 1024L * 10L);
                    addSpecRow(container, "Total Storage", String.valueOf(expTotal), String.valueOf(totalBytes), passTotal);
                }
                if (specs.has("FREE_SPACE")) {
                    long expFree = specs.getLong("FREE_SPACE");
                    boolean passFree = Math.abs(freeBytes - expFree) < (expFree * 0.25);
                    addSpecRow(container, "Free Storage", String.valueOf(expFree), String.valueOf(freeBytes), passFree);
                }

                ActivityManager am = (ActivityManager) getSystemService(ACTIVITY_SERVICE);
                ActivityManager.MemoryInfo memInfo = new ActivityManager.MemoryInfo();
                if (am != null) {
                    am.getMemoryInfo(memInfo);
                    if (specs.has("TOTAL_RAM")) {
                        long expRam = specs.getLong("TOTAL_RAM");
                        boolean passRam = Math.abs(memInfo.totalMem - expRam) < (1024L * 1024L * 50L);
                        addSpecRow(container, "Total RAM", String.valueOf(expRam), String.valueOf(memInfo.totalMem), passRam);
                    }
                }
            }

            // 2. System Properties Multi-Vector Validation
            if (sysProps != null && sysProps.length() > 0) {
                addSectionHeader(container, "--- SYSTEM PROPERTIES (MULTI-VECTOR) ---");

                Class<?> spClass = Class.forName("android.os.SystemProperties");
                Method getMethod = spClass.getMethod("get", String.class, String.class);

                Iterator<String> keys = sysProps.keys();
                while (keys.hasNext()) {
                    String propKey = keys.next();
                    String expectedVal = sysProps.getString(propKey);

                    // Vector 1: SystemProperties Reflection API
                    String valReflection = (String) getMethod.invoke(null, propKey, "");
                    boolean passReflection = valReflection.equals(expectedVal);
                    addPropRow(container, propKey + " [Reflection]", expectedVal, valReflection, passReflection);

                    // Vector 2: Shell Runtime Command Execution (getprop)
                    String valShell = executeShellGetProp(propKey);
                    boolean passShell = valShell.equals(expectedVal);
                    addPropRow(container, propKey + " [Shell Exec]", expectedVal, valShell, passShell);

                    // Vector 3: android.os.Build Mapping (if applicable)
                    String buildMappedVal = getBuildFieldMapping(propKey);
                    if (buildMappedVal != null) {
                        boolean passBuild = buildMappedVal.equals(expectedVal);
                        addPropRow(container, propKey + " [Build Class]", expectedVal, buildMappedVal, passBuild);
                    }
                }
            }

            // Update summary banner text and color dynamically
            updateSummaryBanner();

        } catch (Exception e) {
            addHeader(container, "[ERROR] Verification failed: " + e.getMessage());
            summaryView.setText("[ERROR] Report generation failed.");
            summaryView.setTextColor(0xFF721C24);
            summaryView.setBackgroundColor(0xFFF8D7DA);
        }
    }

    private void updateSummaryBanner() {
        summaryView.setText(passedChecks + " of " + totalChecks + " checks passed!");
        if (passedChecks == totalChecks) {
            summaryView.setTextColor(0xFF155724);
            summaryView.setBackgroundColor(0xFFD4EDDA);
        } else {
            summaryView.setTextColor(0xFF721C24);
            summaryView.setBackgroundColor(0xFFF8D7DA);
        }
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

    private String getBuildFieldMapping(String propKey) {
        switch (propKey) {
            case "ro.product.model": return Build.MODEL;
            case "ro.product.manufacturer": return Build.MANUFACTURER;
            case "ro.product.device": return Build.DEVICE;
            case "ro.product.brand": return Build.BRAND;
            case "ro.product.board": return Build.BOARD;
            case "ro.hardware": return Build.HARDWARE;
            default: return null;
        }
    }

    private void addHeader(LinearLayout container, String text) {
        TextView tv = new TextView(this);
        tv.setText(text);
        tv.setTextSize(15);
        tv.setTypeface(null, android.graphics.Typeface.BOLD);
        tv.setPadding(0, 12, 0, 12);
        container.addView(tv);
    }

    private void addSectionHeader(LinearLayout container, String text) {
        TextView tv = new TextView(this);
        tv.setText(text);
        tv.setTextSize(14);
        tv.setTypeface(null, android.graphics.Typeface.BOLD);
        tv.setPadding(0, 24, 0, 8);
        container.addView(tv);
    }

    private void addSpecRow(LinearLayout container, String name, String expected, String actual, boolean pass) {
        totalChecks++;
        if (pass) passedChecks++;

        String statusIcon = pass ? "[✓ PASS]" : "[✗ FAIL]";
        int color = pass ? 0xFF155724 : 0xFF721C24;
        int bgColor = pass ? 0xFFD4EDDA : 0xFFF8D7DA;

        TextView tv = new TextView(this);
        tv.setText(statusIcon + " " + name + "\n  Exp: " + expected + "\n  Act: " + actual);
        tv.setTextSize(12);
        tv.setPadding(16, 12, 16, 12);
        tv.setTextColor(color);
        tv.setBackgroundColor(bgColor);
        
        LinearLayout.LayoutParams params = new LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT, 
                LinearLayout.LayoutParams.WRAP_CONTENT
        );
        params.setMargins(0, 4, 0, 4);
        tv.setLayoutParams(params);
        container.addView(tv);
    }

    private void addPropRow(LinearLayout container, String label, String expected, String actual, boolean pass) {
        totalChecks++;
        if (pass) passedChecks++;

        String statusIcon = pass ? "[✓]" : "[✗]";
        int color = pass ? 0xFF155724 : 0xFF721C24;
        int bgColor = pass ? 0xFFD4EDDA : 0xFFF8D7DA;

        TextView tv = new TextView(this);
        tv.setText(statusIcon + " " + label + "\n     Exp: " + expected + "\n     Act: " + actual);
        tv.setTextSize(11);
        tv.setPadding(12, 8, 12, 8);
        tv.setTextColor(color);
        tv.setBackgroundColor(bgColor);

        LinearLayout.LayoutParams params = new LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT, 
                LinearLayout.LayoutParams.WRAP_CONTENT
        );
        params.setMargins(0, 3, 0, 3);
        tv.setLayoutParams(params);
        container.addView(tv);
    }
}