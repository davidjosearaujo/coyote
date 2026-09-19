/*
 * Compile with:
 * clang -Wall -Os -pipe -g3 puppy_gumjs.c -o libpuppy.so -shared -fPIC -L. -lfrida-gumjs -lresolv -Wl,-dead_strip
 */

#include "frida-gumjs.h"
#include "config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <dlfcn.h>
#include <limits.h>
#include <android/log.h>

#define LOG_TAG "[PuppyJS]"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

static GumScript *puppy_script = NULL;

static void on_message(GumScript *script, const gchar *message, GBytes *data, gpointer user_data) {
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

static void *puppy_main_thread(void *arg) {
    LOGI("PuppyJS waking up in the background...");
    
    // Initialize GumJS embedded environment
    gum_init_embedded();

    GumScriptBackend *backend = gum_script_backend_obtain_duk();
    GCancellable *cancellable = NULL;
    GError *error = NULL;

    // Dynamically build the JavaScript payload using values from config.h
    GString *js = g_string_new("");

    // 1. Inject Config Macros
    #ifdef SPECS_WIDTH
    g_string_append_printf(js, "const SPECS_WIDTH = %d;\n", SPECS_WIDTH);
    #endif

    #ifdef SPECS_HEIGHT
        g_string_append_printf(js, "const SPECS_HEIGHT = %d;\n", SPECS_HEIGHT);
    #endif

    #ifdef SPECS_TOTAL_SPACE
        g_string_append_printf(js, "const SPECS_TOTAL_SPACE = %llu;\n", SPECS_TOTAL_SPACE);
    #endif

    #ifdef SPECS_FREE_SPACE
        g_string_append_printf(js, "const SPECS_FREE_SPACE = %llu;\n", SPECS_FREE_SPACE);
    #endif

    #ifdef SPECS_TOTAL_RAM
        g_string_append_printf(js, "const SPECS_TOTAL_RAM = %llu;\n", SPECS_TOTAL_RAM);
    #endif

    #ifdef SPECS_AVAIL_RAM
        g_string_append_printf(js, "const SPECS_AVAIL_RAM = %llu;\n", SPECS_AVAIL_RAM);
    #endif

    #ifdef SPECS_SWAP_TOTAL_KB
        g_string_append_printf(js, "const SPECS_SWAP_TOTAL_KB = %d;\n", SPECS_SWAP_TOTAL_KB);
    #endif

    #ifdef DEVICE_CPU_INFO
        g_string_append(js, "const DEVICE_CPU_INFO = `");
        g_string_append(js, DEVICE_CPU_INFO);
        g_string_append(js, "`;\n");
    #endif

    // 2. Inject System Properties Dictionary
    g_string_append(js, "const mockProps = {\n");
    for (size_t i = 0; i < SYSTEM_PROP_OVERRIDES_COUNT; i++) {
        g_string_append_printf(js, "  '%s': '%s',\n", SYSTEM_PROP_OVERRIDES[i].key, SYSTEM_PROP_OVERRIDES[i].value);
    }
    g_string_append(js, "};\n");

    // 3. Inject Frida Hooking Logic
    g_string_append(js,
        "function applyJitter(base, pct) {\n"
        "  const fluctuation = (Math.random() * 2.0) - 1.0;\n"
        "  const jittered = base + (base * pct * fluctuation);\n"
        "  return Math.max(0, Math.round(jittered));\n"
        "}\n\n"

        "// --- System Properties ---\n"
        "const sysPropGet = Module.findExportByName(null, '__system_property_get');\n"
        "if (sysPropGet) {\n"
        "  Interceptor.attach(sysPropGet, {\n"
        "    onEnter: function(args) {\n"
        "      this.name = args[0].readUtf8String();\n"
        "      this.valPtr = args[1];\n"
        "    },\n"
        "    onLeave: function(retval) {\n"
        "      if (mockProps[this.name] !== undefined) {\n"
        "        this.valPtr.writeUtf8String(mockProps[this.name]);\n"
        "        retval.replace(mockProps[this.name].length);\n"
        "      }\n"
        "    }\n"
        "  });\n"
        "}\n\n"
    );

    #if defined(SPECS_WIDTH) || defined(SPECS_HEIGHT)
    g_string_append(js, "// --- Native Window ---\n");
    #ifdef SPECS_WIDTH
    g_string_append(js,
        "const getWidth = Module.findExportByName('libnativewindow.so', 'ANativeWindow_getWidth');\n"
        "if (getWidth) {\n"
        "  Interceptor.replace(getWidth, new NativeCallback(() => SPECS_WIDTH, 'int32', ['pointer']));\n"
        "}\n"
    );
    #endif
    #ifdef SPECS_HEIGHT
    g_string_append(js,
        "const getHeight = Module.findExportByName('libnativewindow.so', 'ANativeWindow_getHeight');\n"
        "if (getHeight) {\n"
        "  Interceptor.replace(getHeight, new NativeCallback(() => SPECS_HEIGHT, 'int32', ['pointer']));\n"
        "}\n"
    );
    #endif
    #endif
    
    #if defined(DEVICE_CPU_INFO) || (defined(SPECS_TOTAL_RAM) && defined(SPECS_AVAIL_RAM) && defined(SPECS_SWAP_TOTAL_KB))
    g_string_append(js, 
        "// --- Memory & Storage Faking (Syscall wrapper for memfd) ---\n"
        "const syscall = new NativeFunction(Module.findExportByName(null, 'syscall'), 'int', ['int', 'pointer', 'int']);\n"
        "const write = new NativeFunction(Module.findExportByName(null, 'write'), 'ssize_t', ['int', 'pointer', 'size_t']);\n"
        "const lseek = new NativeFunction(Module.findExportByName(null, 'lseek'), 'off_t', ['int', 'off_t', 'int']);\n"
        "const NR_memfd_create = Process.arch === 'arm64' ? 279 : (Process.arch === 'arm' ? 385 : 319);\n\n"

        "function createMemfd(name, content) {\n"
        "  const namePtr = Memory.allocUtf8String(name);\n"
        "  const fd = syscall(NR_memfd_create, namePtr, 1);\n"
        "  if (fd >= 0) {\n"
        "    const contentPtr = Memory.allocUtf8String(content);\n"
        "    write(fd, contentPtr, content.length);\n"
        "    lseek(fd, 0, 0);\n"
        "  }\n"
        "  return fd;\n"
        "}\n\n"

        "const openat = Module.findExportByName(null, 'openat');\n"
        "if (openat) {\n"
        "  Interceptor.attach(openat, {\n"
        "    onEnter: function(args) {\n"
        "      this.path = args[1].readUtf8String();\n"
        "      this.isTarget = false;\n"
    );
    #ifdef DEVICE_CPU_INFO
    g_string_append(js,
        "      if (this.path === '/proc/cpuinfo') {\n"
        "        this.isTarget = true;\n"
        "        this.fakeContent = DEVICE_CPU_INFO;\n"
        "        this.fakeName = 'cpuinfo_fake';\n"
        "      } else "
    );
    #endif
    #if defined(SPECS_TOTAL_RAM) && defined(SPECS_AVAIL_RAM) && defined(SPECS_SWAP_TOTAL_KB)
    g_string_append(js,
        "      if (this.path === '/proc/meminfo') {\n"
        "        this.isTarget = true;\n"
        "        this.fakeName = 'meminfo_fake';\n"
        "        let avail = applyJitter(SPECS_AVAIL_RAM / 1024, 0.02);\n"
        "        let free = applyJitter(avail * 0.35, 0.05);\n"
        "        this.fakeContent = `MemTotal:        ${Math.floor(SPECS_TOTAL_RAM / 1024)} kB\\nMemFree:         ${free} kB\\nMemAvailable:    ${avail} kB\\nSwapTotal:       ${SPECS_SWAP_TOTAL_KB} kB\\n`;\n"
        "      } else "
    );
    #endif
    // Catch-all selinux bypass and close the hook logic safely
    g_string_append(js,
        "      if (this.path === '/sys/fs/selinux/enforce') {\n"
        "        this.isTarget = true;\n"
        "        this.fakeContent = '1\\n';\n"
        "        this.fakeName = 'selinux_fake';\n"
        "      }\n"
        "    },\n"
        "    onLeave: function(retval) {\n"
        "      if (this.isTarget) {\n"
        "        const fakeFd = createMemfd(this.fakeName, this.fakeContent);\n"
        "        if (fakeFd >= 0) retval.replace(fakeFd);\n"
        "        console.log('PuppyJS intercepted openat for: ' + this.path);\n"
        "      }\n"
        "    }\n"
        "  });\n"
        "}\n"
    );
    #endif

    // Compile and run script
    puppy_script = gum_script_backend_create_sync(backend, "puppy", js->str, cancellable, &error);
    g_string_free(js, TRUE);

    if (error != NULL) {
        LOGE("PuppyJS lost the scent: %s", error->message);
        g_error_free(error);
        return NULL;
    }

    gum_script_set_message_handler(puppy_script, on_message, NULL, NULL);
    gum_script_load_sync(puppy_script, cancellable);

    LOGI("Woof! System property hooks successfully buried and active via GumJS.");

    // Keep the thread alive to process GumJS background events
    GMainContext *context = g_main_context_get_thread_default();
    while (TRUE) {
        if (g_main_context_pending(context)) {
            g_main_context_iteration(context, TRUE);
        } else {
            usleep(100000); // 100ms sleep
        }
    }

    return NULL;
}

__attribute__((constructor)) void puppy_init(void) {
    LOGI("PuppyJS barking at process entry (PID: %d)...", getpid());

    pthread_t thread;
    if (pthread_create(&thread, NULL, puppy_main_thread, NULL) != 0) {
        LOGE("PuppyJS failed to leave the kennel!");
    } else {
        pthread_detach(thread);
        LOGI("PuppyJS slipped off the leash into the background.");
    }
}