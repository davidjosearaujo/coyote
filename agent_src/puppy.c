#include "frida-gum.h"
#include "config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <dlfcn.h>
#include <sys/system_properties.h>
#include <android/log.h>

#include <android/native_window.h>

#define LOG_TAG "[Puppy]"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

typedef int32_t (*ANativeWindowGetWidthFunc)(ANativeWindow *window);
typedef int32_t (*ANativeWindowGetHeightFunc)(ANativeWindow *window);

static ANativeWindowGetWidthFunc orig_anativewindow_get_width = NULL;
static ANativeWindowGetHeightFunc orig_anativewindow_get_height = NULL;

static int32_t my_anativewindow_get_width(ANativeWindow *window) {
    LOGI("Puppy intercepted ANativeWindow_getWidth! Spoofing width.");
    return SPECS_WIDTH; // Replace with your target width spec
}

static int32_t my_anativewindow_get_height(ANativeWindow *window) {
    LOGI("Puppy intercepted ANativeWindow_getHeight! Spoofing height.");
    return SPECS_HEIGHT; // Replace with your target height spec
}

typedef int (*SystemPropertyGetFunc)(const char *name, char *value);
static SystemPropertyGetFunc orig_system_property_get = NULL;

static int my_system_property_get(const char *name, char *value) {
    if (name != NULL && value != NULL) {
        for (size_t i = 0; i < SYSTEM_PROP_OVERRIDES_COUNT; i++) {
            if (strcmp(name, SYSTEM_PROP_OVERRIDES[i].key) == 0) {
                strcpy(value, SYSTEM_PROP_OVERRIDES[i].value);
                return (int)strlen(SYSTEM_PROP_OVERRIDES[i].value);
            }
        }
    }
    return orig_system_property_get(name, value);
}

static void* puppy_main_thread(void* arg) {
    sleep(3);
    
    LOGI("Puppy waking up in the background... sniffing out environment.");
    gum_init_embedded();

    GumInterceptor *interceptor = gum_interceptor_obtain();
    if (interceptor == NULL) {
        LOGE("Puppy lost the scent: failed to obtain GumInterceptor.");
        return NULL;
    }

    gpointer target_prop_address = GSIZE_TO_POINTER(dlsym(RTLD_DEFAULT, "__system_property_get"));
    
    void *libnativewindow = dlopen("libnativewindow.so", RTLD_LAZY);
    gpointer target_width_address = libnativewindow ? dlsym(libnativewindow, "ANativeWindow_getWidth") : NULL;
    gpointer target_height_address = libnativewindow ? dlsym(libnativewindow, "ANativeWindow_getHeight") : NULL;
    
    gum_interceptor_begin_transaction(interceptor);

    if (target_prop_address) {
        gum_interceptor_replace(
            interceptor,
            target_prop_address, 
            GSIZE_TO_POINTER(my_system_property_get),
            (gpointer *)&orig_system_property_get,
            NULL
        );
    }

    if (target_width_address) {
        gum_interceptor_replace(
            interceptor,
            target_width_address, 
            GSIZE_TO_POINTER(my_anativewindow_get_width),
            (gpointer *)&orig_anativewindow_get_width,
            NULL
        );
    }

    if (target_height_address) {
        gum_interceptor_replace(
            interceptor,
            target_height_address, 
            GSIZE_TO_POINTER(my_anativewindow_get_height),
            (gpointer *)&orig_anativewindow_get_height,
            NULL
        );
    }

    gum_interceptor_end_transaction(interceptor);

    LOGI("Woof! System property hooks successfully buried and active.");
    return NULL;
}

__attribute__((constructor)) void puppy_init(void) {
    LOGI("Puppy barking at process entry (PID: %d)...", getpid());
    
    pthread_t thread;
    if (pthread_create(&thread, NULL, puppy_main_thread, NULL) != 0) {
        LOGE("Puppy failed to leave the kennel!");
    } else {
        pthread_detach(thread);
        LOGI("Puppy slipped off the leash into the background.");
    }
}