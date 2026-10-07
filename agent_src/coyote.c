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
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)

// Declare external functions
extern void apply_native_gum_hooks();
extern void apply_gumjs_hooks();

static void* coyote_master_thread(void* arg) {
    LOGI("Master thread waking up...");
    
    // Initialize Frida engine once
    gum_init_embedded();

    // 1. Apply C-level hooks immediately
    apply_native_gum_hooks();

    // 2. Load JS hooks and block the thread with the event loop
    apply_gumjs_hooks();

    return NULL;
}

__attribute__((constructor)) void coyote_init(void) {
    LOGI("Coyote loaded (PID: %d)", getpid());
    pthread_t thread;
    if (pthread_create(&thread, NULL, coyote_master_thread, NULL) == 0) {
        pthread_detach(thread);
    }
}