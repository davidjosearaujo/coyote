#include "frida-gumjs.h"
#include "config.h"
#include "agent.h"

#include <android/log.h>
#include <pthread.h>
#include <unistd.h>

#define LOG_TAG "[PuppyJS]"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

static GumScript *puppy_script = NULL;

static void on_message(const gchar *message, GBytes *data, gpointer user_data) {
    LOGI("PuppyJS message: %s", message);
}

void apply_gumjs_hooks() {
    GMainContext *context = g_main_context_new();
    g_main_context_push_thread_default(context);
    GMainLoop *loop = g_main_loop_new(context, FALSE);
    
    GumScriptBackend *backend = gum_script_backend_obtain_qjs();
    GError *error = NULL;

    GString *js = g_string_new("");

    g_string_append(js, "console.log('[+] Injecting C-macros into JS globals...');\n");
    g_string_append(js, "var global = globalThis;\n");

    // ============================================
    // BLOCK 1: Display Metrics Hooks
    // ============================================
    #ifdef SPECS_WIDTH
    g_string_append_printf(js, "global.SPECS_WIDTH = %d;\n", SPECS_WIDTH);
    #endif

    #ifdef SPECS_HEIGHT
    g_string_append_printf(js, "global.SPECS_HEIGHT = %d;\n", SPECS_HEIGHT);
    #endif

    #ifdef SPECS_DENSITY
    g_string_append_printf(js, "global.SPECS_DENSITY = parseFloat(\"%s\");\n", SPECS_DENSITY);
    #endif

    #ifdef SPECS_DENSITY_DPI
    g_string_append_printf(js, "global.SPECS_DENSITY_DPI = %d;\n", SPECS_DENSITY_DPI);
    #endif

    #ifdef SPECS_XDPI
    g_string_append_printf(js, "global.SPECS_XDPI = parseFloat(\"%s\");\n", SPECS_XDPI);
    #endif

    #ifdef SPECS_YDPI
    g_string_append_printf(js, "global.SPECS_YDPI = parseFloat(\"%s\");\n", SPECS_YDPI);
    #endif

    #ifdef SPECS_TOTAL_RAM
    g_string_append_printf(js, "global.SPECS_TOTAL_RAM = %llu;\n", SPECS_TOTAL_RAM);
    #endif

    #ifdef SPECS_RAM_THRESHOLD
    g_string_append_printf(js, "global.SPECS_RAM_THRESHOLD = %llu;\n", SPECS_RAM_THRESHOLD);
    #endif

    #ifdef SPECS_AVAIL_RAM
    g_string_append_printf(js, "global.SPECS_AVAIL_RAM = %llu;\n", SPECS_AVAIL_RAM);
    #endif

    #ifdef SPECS_TOTAL_SPACE
    g_string_append_printf(js, "global.SPECS_TOTAL_SPACE = %llu;\n", SPECS_TOTAL_SPACE);
    #endif

    #ifdef SPECS_FREE_SPACE
    g_string_append_printf(js, "global.SPECS_FREE_SPACE = %llu;\n", SPECS_FREE_SPACE);
    #endif

    // ============================================
    // BLOCK 3: RAM Memory Macro Injection
    // ============================================
    #ifdef SPECS_TOTAL_RAM
    g_string_append_printf(js, "global.SPECS_TOTAL_RAM = %llu;\n", SPECS_TOTAL_RAM);
    #endif

    #ifdef SPECS_RAM_THRESHOLD
    g_string_append_printf(js, "global.SPECS_RAM_THRESHOLD = %llu;\n", SPECS_RAM_THRESHOLD);
    #endif

    #ifdef SPECS_AVAIL_RAM
    g_string_append_printf(js, "global.SPECS_AVAIL_RAM = %llu;\n", SPECS_AVAIL_RAM);
    #endif

    // ============================================
    // BLOCK 4: Battery Cloaking Macro Injection
    // ============================================
    #ifdef SPECS_BATTERY_LEVEL
    g_string_append_printf(js, "global.SPECS_BATTERY_LEVEL = %d;\n", SPECS_BATTERY_LEVEL);
    #endif

    // ============================================
    // BLOCK 5: Time Cloaking Macro Injection
    // ============================================
    #ifdef SYS_PROP_PERSIST_SYS_TIMEZONE
    g_string_append_printf(js, "global.SYS_PROP_PERSIST_SYS_TIMEZONE = '%s';\n", SYS_PROP_PERSIST_SYS_TIMEZONE);
    #endif

    // ============================================
    // BLOCK 6: Install Referrer Macro Injection
    // ============================================
    #ifdef ATTRIBUTION_INSTALL_REFERRER
    g_string_append_printf(js, "global.ATTRIBUTION_INSTALL_REFERRER = '%s';\n", ATTRIBUTION_INSTALL_REFERRER);
    #endif

    // ============================================
    // BLOCK 8: FLAG_SECURE Bypass Macro Injection
    // ============================================
    #ifdef BYPASS_FLAG_SECURE
    g_string_append(js, "global.BYPASS_FLAG_SECURE = true;\n");
    #endif

    // ============================================
    // BLOCK 9: Telephony Carrier Macro Injection
    // ============================================
    #ifdef TELECOM_MCC
    g_string_append_printf(js, "global.TELECOM_MCC = '%s';\n", TELECOM_MCC);
    #endif

    #ifdef TELECOM_MNC
    g_string_append_printf(js, "global.TELECOM_MNC = '%s';\n", TELECOM_MNC);
    #endif

    #ifdef TELECOM_OPERATOR_NAME
    g_string_append_printf(js, "global.TELECOM_OPERATOR_NAME = '%s';\n", TELECOM_OPERATOR_NAME);
    #endif

    #ifdef TELECOM_COUNTRY_ISO
    g_string_append_printf(js, "global.TELECOM_COUNTRY_ISO = '%s';\n", TELECOM_COUNTRY_ISO);
    #endif

    #ifdef TELECOM_LANGUAGE
    g_string_append_printf(js, "global.TELECOM_LANGUAGE = '%s';\n", TELECOM_LANGUAGE);
    #endif

    #ifdef TELECOM_PHONE_CODE
    g_string_append_printf(js, "global.TELECOM_PHONE_CODE = '%s';\n", TELECOM_PHONE_CODE);
    #endif

    #ifdef TELECOM_SIM_STATE_NUM
    g_string_append_printf(js, "global.TELECOM_SIM_STATE_NUM = %d;\n", TELECOM_SIM_STATE_NUM);
    #endif

    #ifdef TELECOM_MCC_COUNTRY_NAME
    g_string_append_printf(js, "global.TELECOM_MCC_COUNTRY_NAME = '%s';\n", TELECOM_MCC_COUNTRY_NAME);
    #endif

    #ifdef TELECOM_IMEI_PREFIX
    g_string_append_printf(js, "global.TELECOM_IMEI_PREFIX = '%s';\n", TELECOM_IMEI_PREFIX);
    #endif

    #ifdef TELECOM_NETWORK_TYPE
    g_string_append_printf(js, "global.TELECOM_NETWORK_TYPE = '%s';\n", TELECOM_NETWORK_TYPE);
    #endif

    // ============================================
    // BLOCK 10: Development & Debug Settings Macro Injection
    // ============================================
    #ifdef SYS_PROP_RO_PRODUCT_MODEL
    g_string_append_printf(js, "global.SYS_PROP_RO_PRODUCT_MODEL = '%s';\n", SYS_PROP_RO_PRODUCT_MODEL);
    #endif

    #ifdef SYS_PROP_RO_BUILD_DISPLAY_ID
    g_string_append_printf(js, "global.SYS_PROP_RO_BUILD_DISPLAY_ID = '%s';\n", SYS_PROP_RO_BUILD_DISPLAY_ID);
    #endif

    #ifdef SYS_PROP_PERSIST_SYS_LOCALE
    g_string_append_printf(js, "global.SYS_PROP_PERSIST_SYS_LOCALE = '%s';\n", SYS_PROP_PERSIST_SYS_LOCALE);
    #endif

    #ifdef NETWORK_NET_TYPE
    g_string_append_printf(js, "global.NETWORK_NET_TYPE = %d;\n", NETWORK_NET_TYPE);
    #endif

    #ifdef DEVICE_KEYBOARD
    g_string_append_printf(js, "global.DEVICE_KEYBOARD = '%s';\n", DEVICE_KEYBOARD);
    #endif

    // ============================================
    // BLOCK 11: Locale & Timezone Macro Injection
    // ============================================
    #ifdef SYS_PROP_PERSIST_SYS_LANGUAGE
    g_string_append_printf(js, "global.SYS_PROP_PERSIST_SYS_LANGUAGE = '%s';\n", SYS_PROP_PERSIST_SYS_LANGUAGE);
    #endif

    #ifdef SYS_PROP_PERSIST_SYS_LOCALE
    g_string_append_printf(js, "global.SYS_PROP_PERSIST_SYS_LOCALE = '%s';\n", SYS_PROP_PERSIST_SYS_LOCALE);
    #endif

    #ifdef SYS_PROP_PERSIST_SYS_TIMEZONE
    g_string_append_printf(js, "global.SYS_PROP_PERSIST_SYS_TIMEZONE = '%s';\n", SYS_PROP_PERSIST_SYS_TIMEZONE);
    #endif

    // ============================================
    // BLOCK 12: Network Info Macro Injection
    // ============================================
    #ifdef NETWORK_NET_TYPE
    g_string_append_printf(js, "global.NETWORK_NET_TYPE = %d;\n", NETWORK_NET_TYPE);
    #endif

    // ============================================
    // BLOCK 13: Network & VPN Macro Injection
    // ============================================
    #ifdef NETWORK_NET_TYPE
    g_string_append_printf(js, "global.NETWORK_NET_TYPE = %d;\n", NETWORK_NET_TYPE);
    #endif

    #ifdef NETWORK_MOBILE_DATA_LOCAL_IP
    g_string_append_printf(js, "global.NETWORK_MOBILE_DATA_LOCAL_IP = '%s';\n", NETWORK_MOBILE_DATA_LOCAL_IP);
    #endif

    #ifdef NETWORK_WIFI_LOCAL_IP
    g_string_append_printf(js, "global.NETWORK_WIFI_LOCAL_IP = '%s';\n", NETWORK_WIFI_LOCAL_IP);
    #endif

    // ============================================
    // BLOCK 16: User-Agent Spoofing Macro Injection
    // ============================================
    #ifdef DEVICE_MODEL
    g_string_append_printf(js, "global.DEVICE_MODEL = '%s';\n", DEVICE_MODEL);
    #endif

    #ifdef DEVICE_DISPLAY_ID
    g_string_append_printf(js, "global.DEVICE_DISPLAY_ID = '%s';\n", DEVICE_DISPLAY_ID);
    #endif

    // ============================================
    // BLOCK 17: Package Manager Spoofing Macro Injection
    // ============================================
    #ifdef PACKAGE_NAME
    g_string_append_printf(js, "global.PACKAGE_NAME = '%s';\n", PACKAGE_NAME);
    #else
    g_string_append(js, "global.PACKAGE_NAME = 'com.border.patrol';\n");
    #endif

    // 2. Append the massive frida-java-bridge bundle
    g_string_append_len(js, (const gchar *)agent_js, agent_js_len);

    // 3. Compile and execute
    puppy_script = gum_script_backend_create_sync(backend, "puppy_js", js->str, NULL, NULL, &error);
    
    if (error != NULL) {
        LOGE("PuppyJS script compilation failed: %s", error->message);
        g_error_free(error);
        g_string_free(js, TRUE);

        g_main_loop_unref(loop);
        g_main_context_pop_thread_default(context);
        g_main_context_unref(context);
        return;
    }

    gum_script_set_message_handler(puppy_script, on_message, NULL, NULL);
    gum_script_load_sync(puppy_script, NULL);
    g_string_free(js, TRUE);

    // 2. Block the thread efficiently using GMainLoop
    LOGI("Starting GMainLoop for GumJS...");
    g_main_loop_run(loop);

    // 3. Cleanup (unreachable unless g_main_loop_quit is called)
    g_main_loop_unref(loop);
    g_main_context_pop_thread_default(context);
    g_main_context_unref(context);
}