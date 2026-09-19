#include "frida-gumjs.h"
#include "config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <android/log.h>

#define LOG_TAG "[PuppyJS]"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

static GumScript *puppy_script = NULL;

static void on_message(const gchar *message, GBytes *data, gpointer user_data) {
    JsonParser *parser = json_parser_new();
    json_parser_load_from_data(parser, message, -1, NULL);
    JsonObject *root = json_node_get_object(json_parser_get_root(parser));

    const gchar *type = json_object_get_string_member(root, "type");
    if (strcmp(type, "log") == 0) {
        const gchar *log_message = json_object_get_string_member(root, "payload");
        LOGI("%s", log_message);
    } else {
        LOGI("Message: %s", message);
    }
    g_object_unref(parser);
}

void apply_gumjs_hooks() {
    LOGI("PuppyJS waking up to handle Java-layer metric & RAM mocks...");

    GumScriptBackend *backend = gum_script_backend_obtain_qjs();
    GCancellable *cancellable = NULL;
    GError *error = NULL;

    GString *js = g_string_new("");

    // 1. Inject Config Macros for Display & RAM
    #ifdef SPECS_WIDTH
    g_string_append_printf(js, "const SPECS_WIDTH = %d;\n", SPECS_WIDTH);
    #endif

    #ifdef SPECS_HEIGHT
    g_string_append_printf(js, "const SPECS_HEIGHT = %d;\n", SPECS_HEIGHT);
    #endif

    #ifdef SPECS_DENSITY
    // Handle both numeric float or string format safely
    g_string_append_printf(js, "const SPECS_DENSITY = %s;\n", SPECS_DENSITY);
    #endif

    #ifdef SPECS_DENSITY_DPI
    g_string_append_printf(js, "const SPECS_DENSITY_DPI = %d;\n", SPECS_DENSITY_DPI);
    #endif

    #ifdef SPECS_XDPI
    g_string_append_printf(js, "const SPECS_XDPI = %s;\n", SPECS_XDPI);
    #endif

    #ifdef SPECS_YDPI
    g_string_append_printf(js, "const SPECS_YDPI = %s;\n", SPECS_YDPI);
    #endif

    #ifdef SPECS_TOTAL_RAM
    g_string_append_printf(js, "const SPECS_TOTAL_RAM = %llu;\n", SPECS_TOTAL_RAM);
    #endif

    // 2. Inject Frida Java Interception Logic for DisplayMetrics & ActivityManager.MemoryInfo
    g_string_append(js,
        "Java.perform(function() {\n"
        "  console.log('[+] Frida JS attaching Java hooks for Display & RAM...');\n"
        "  \n"
        "  // Hook Display metrics (Display.getRealMetrics / getMetrics)\n"
        "  try {\n"
        "    const Display = Java.use('android.view.Display');\n"
        "    const patchMetrics = function(metrics) {\n"
        "      if (metrics !== null) {\n"
        "        if (typeof SPECS_WIDTH !== 'undefined') metrics.widthPixels.value = SPECS_WIDTH;\n"
        "        if (typeof SPECS_HEIGHT !== 'undefined') metrics.heightPixels.value = SPECS_HEIGHT;\n"
        "        if (typeof SPECS_DENSITY !== 'undefined') metrics.density.value = SPECS_DENSITY;\n"
        "        if (typeof SPECS_DENSITY_DPI !== 'undefined') metrics.densityDpi.value = SPECS_DENSITY_DPI;\n"
        "        if (typeof SPECS_XDPI !== 'undefined') metrics.xdpi.value = SPECS_XDPI;\n"
        "        if (typeof SPECS_YDPI !== 'undefined') metrics.ydpi.value = SPECS_YDPI;\n"
        "      }\n"
        "    };\n"
        "    Display.getRealMetrics.overload('android.util.DisplayMetrics').implementation = function(metrics) {\n"
        "      this.getRealMetrics(metrics);\n"
        "      patchMetrics(metrics);\n"
        "    };\n"
        "    Display.getMetrics.overload('android.util.DisplayMetrics').implementation = function(metrics) {\n"
        "      this.getMetrics(metrics);\n"
        "      patchMetrics(metrics);\n"
        "    };\n"
        "  } catch (e) {\n"
        "    console.log('[-] Error hooking Display metrics: ' + e);\n"
        "  }\n"
        "  \n"
        "  // Hook ActivityManager MemoryInfo totalMem\n"
        "  try {\n"
        "    const ActivityManager = Java.use('android.app.ActivityManager');\n"
        "    ActivityManager.getMemoryInfo.overload('android.app.ActivityManager$MemoryInfo').implementation = function(mi) {\n"
        "      this.getMemoryInfo(mi);\n"
        "      if (mi !== null && typeof SPECS_TOTAL_RAM !== 'undefined') {\n"
        "        mi.totalMem.value = SPECS_TOTAL_RAM;\n"
        "      }\n"
        "    };\n"
        "  } catch (e) {\n"
        "    console.log('[-] Error hooking ActivityManager MemoryInfo: ' + e);\n"
        "  }\n"
        "});\n"
    );

    // Compile and run script
    puppy_script = gum_script_backend_create_sync(backend, "puppy_js", js->str, NULL, cancellable, &error);
    g_string_free(js, TRUE);

    if (error != NULL) {
        LOGE("PuppyJS script compilation failed: %s", error->message);
        g_error_free(error);
        return;
    }

    gum_script_set_message_handler(puppy_script, on_message, NULL, NULL);
    gum_script_load_sync(puppy_script, cancellable);

    LOGI("Woof! Display and RAM Java hooks successfully deployed via GumJS.");

    // Keep the thread alive to process GumJS background events
    GMainContext *context = g_main_context_get_thread_default();
    while (TRUE) {
        if (g_main_context_pending(context)) {
            g_main_context_iteration(context, TRUE);
        } else {
            usleep(100000); // 100ms sleep
        }
    }
}