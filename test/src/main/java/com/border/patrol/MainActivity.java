package com.border.patrol;

import android.app.Activity;
import android.app.ActivityManager;
import android.graphics.Typeface;
import android.graphics.drawable.GradientDrawable;
import android.os.Build;
import android.os.Bundle;
import android.os.Environment;
import android.os.StatFs;
import android.util.DisplayMetrics;
import android.view.View;
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
    private static final int COLOR_BG_PASS = 0xFFECFDF5;      // Soft emerald green
    private static final int COLOR_TEXT_BG_PASS = 0xFF065F46; // Dark emerald green
    private static final int COLOR_BG_FAIL = 0xFFFEF2F2;      // Soft rose red
    private static final int COLOR_TEXT_BG_FAIL = 0xFF991B1B; // Dark rose red
    private static final int COLOR_TEXT_PRIMARY = 0xFF0F172A; // Dark slate
    private static final int COLOR_TEXT_SECONDARY = 0xFF64748B; // Light slate
    private static final int COLOR_ACCENT_BG = 0xFFF1F5F9;    // Cool grey card accent

    private int totalChecks = 0;
    private int passedChecks = 0;
    private TextView summaryView;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);

        ScrollView scrollView = new ScrollView(this);
        scrollView.setBackgroundColor(0xFFF8FAFC); // Clean off-white background

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
                }

                hwHeader.setText("Hardware Specs  •  " + hwPassed + " / " + hwTotal + " Passed");
                addPassedDropdown(container, hwPassedContainer, hwPassed);
            }

            // 2. System Properties Grouped by Vector
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

                Iterator<String> keysBuild = sysProps.keys();
                while (keysBuild.hasNext()) {
                    String propKey = keysBuild.next();
                    String expectedVal = sysProps.getString(propKey);
                    String buildMappedVal = getBuildFieldMapping(propKey);
                    if (buildMappedVal != null) {
                        boolean passBuild = buildMappedVal.equals(expectedVal);
                        addPropRow(passBuild ? buildPassedContainer : container, propKey, expectedVal, buildMappedVal, passBuild);
                        buildTotal++; if (passBuild) buildPassed++;
                    }
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
        tv.setTypeface(Typeface.MONOSPACE); // Monospace for clean technical data display
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