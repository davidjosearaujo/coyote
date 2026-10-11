#include "frida-gumjs.h"

#include <stdio.h>          // LOGI macro (indirectly)
#include <stdlib.h>         // could be removed but harmless
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <dlfcn.h>
#include <limits.h>
#include <android/log.h> 

#define LOG_TAG "[Coyote]"
#ifdef COYOTE_DEBUG
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#else
#define LOGI(...) ((void)0)
#endif

// Declare external functions
extern void apply_native_gum_hooks();
extern void apply_gumjs_hooks();

// Decide whether to run the heavy JS/Java layer in THIS process.
//
// The `wrap` + LD_PRELOAD injection fires in every process Zygote forks for the
// package, but app detection runs in the main UI process. Auxiliary processes
// are named "<pkg>:<suffix>" (e.g. "com.app:remote"), the main one is the bare
// package name. We run the cheap native hooks everywhere, but spin up the
// QuickJS runtime + frida-java-bridge + GMainLoop only in the main process.
//
// The framework sets the process name (/proc/self/cmdline argv0) shortly after
// this library loads, so we poll briefly until a package-style name appears,
// then classify. If we can't tell, we default to running (never lose coverage).
// Define COYOTE_JS_ALL_PROCESSES to force the JS layer into every process.
static int coyote_should_run_js(void) {
#ifdef COYOTE_JS_ALL_PROCESSES
    return 1;
#else
    char name[256];
    for (int i = 0; i < 100; i++) { // up to ~1s
        name[0] = '\0';
        FILE *f = fopen("/proc/self/cmdline", "r");
        if (f) {
            size_t n = fread(name, 1, sizeof(name) - 1, f);
            fclose(f);
            if (n > 0) name[n] = '\0';
        }
        if (strchr(name, '.') != NULL) {       // a package-style name is set
            return strchr(name, ':') == NULL;  // main process has no ':' suffix
        }
        usleep(10 * 1000); // 10ms
    }
    return 1; // couldn't determine the process name -> run (safe default)
#endif
}

static void* coyote_master_thread(void* arg) {
    LOGI("Master thread waking up...");

    // Initialize Frida engine once
    gum_init_embedded();

    // 1. Apply C-level hooks immediately (cheap; every process).
    apply_native_gum_hooks();

    // 2. Load JS hooks and block the thread with the event loop, but only in
    //    the main process. Auxiliary processes skip the heavy JS layer and this
    //    thread simply exits.
    if (coyote_should_run_js()) {
        apply_gumjs_hooks();
    } else {
        LOGI("Auxiliary process; skipping JS layer.");
    }

    return NULL;
}

__attribute__((constructor)) void coyote_init(void) {
    LOGI("Coyote loaded (PID: %d)", getpid());
    pthread_t thread;
    if (pthread_create(&thread, NULL, coyote_master_thread, NULL) == 0) {
        pthread_detach(thread);
    }
}