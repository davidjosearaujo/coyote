#include "frida-gum.h"
#include "config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <dlfcn.h>
#include <sys/vfs.h>
#include <sys/statvfs.h>
#include <sys/sysinfo.h>
#include <math.h>

#include <sys/system_properties.h>

#include <android/log.h>
#include <android/native_window.h>

#define LOG_TAG "[Puppy]"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

typedef int (*SystemPropertyGetFunc)(const char *name, char *value);

typedef int32_t (*ANativeWindowGetWidthFunc)(ANativeWindow *window);
typedef int32_t (*ANativeWindowGetHeightFunc)(ANativeWindow *window);

typedef int (*StatfsFunc)(const char *path, struct statfs *buf);
typedef int (*Statfs64Func)(const char *path, struct statfs64 *buf);
typedef int (*StatvfsFunc)(const char *path, struct statvfs *buf);
typedef int (*Statvfs64Func)(const char *path, struct statvfs64 *buf);
typedef int (*SysinfoFunc)(struct sysinfo *info);

static SystemPropertyGetFunc orig_system_property_get = NULL;

static ANativeWindowGetWidthFunc orig_anativewindow_get_width = NULL;
static ANativeWindowGetHeightFunc orig_anativewindow_get_height = NULL;

static StatfsFunc orig_statfs = NULL;
static Statfs64Func orig_statfs64 = NULL;
static StatvfsFunc orig_statvfs = NULL;
static Statvfs64Func orig_statvfs64 = NULL;
static SysinfoFunc orig_sysinfo = NULL;

#ifdef SYSTEM_PROP_OVERRIDES_COUNT
static int hook_system_property_get(const char *name, char *value) {
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
#endif

#ifdef SPECS_WIDTH
static int32_t hook_anativewindow_get_width(ANativeWindow *window) {
    LOGI("Puppy intercepted ANativeWindow_getWidth! Spoofing width.");
    return SPECS_WIDTH;
}
#endif

#ifdef SPECS_HEIGHT
static int32_t hook_anativewindow_get_height(ANativeWindow *window) {
    LOGI("Puppy intercepted ANativeWindow_getHeight! Spoofing height.");
    return SPECS_HEIGHT;
}
#endif

static long long get_realistic_jitter(long long base_val, double percentage) {
    double fluctuation = ((double)rand() / RAND_MAX) * 2.0 - 1.0; 
    return base_val + (long long)(base_val * percentage * fluctuation);
}

#if defined(SPECS_FREE_SPACE) && defined(SPECS_TOTAL_SPACE)
static int hook_statfs(const char *path, struct statfs *buf) {
    int ret = orig_statfs(path, buf);
    if (ret == 0 && buf != NULL) {
        long long jittered_free = get_realistic_jitter(SPECS_FREE_SPACE, 0.0005);
        buf->f_blocks = SPECS_TOTAL_SPACE / buf->f_bsize;
        buf->f_bfree = jittered_free / buf->f_bsize;
        buf->f_bavail = buf->f_bfree; 
        LOGI("Puppy spoofed statfs for path: %s", path);
    }
    return ret;
}
static int hook_statfs64(const char *path, struct statfs64 *buf) {
    int ret = orig_statfs64(path, buf);
    if (ret == 0 && buf != NULL) {
        long long jittered_free = get_realistic_jitter(SPECS_FREE_SPACE, 0.0005);
        buf->f_blocks = SPECS_TOTAL_SPACE / buf->f_bsize;
        buf->f_bfree = jittered_free / buf->f_bsize;
        buf->f_bavail = buf->f_bfree; 
        LOGI("Puppy spoofed statfs64 for path: %s", path);
    }
    return ret;
}
static int hook_statvfs(const char *path, struct statvfs *buf) {
    int ret = orig_statvfs(path, buf);
    if (ret == 0 && buf != NULL) {
        long long jittered_free = get_realistic_jitter(SPECS_FREE_SPACE, 0.0005);
        buf->f_blocks = SPECS_TOTAL_SPACE / buf->f_frsize;
        buf->f_bfree = jittered_free / buf->f_frsize;
        buf->f_bavail = buf->f_bfree;
        LOGI("Puppy spoofed statvfs for path: %s", path);
    }
    return ret;
}
static int hook_statvfs64(const char *path, struct statvfs64 *buf) {
    int ret = orig_statvfs64(path, buf);
    if (ret == 0 && buf != NULL) {
        long long jittered_free = get_realistic_jitter(SPECS_FREE_SPACE, 0.0005);
        buf->f_blocks = SPECS_TOTAL_SPACE / buf->f_frsize;
        buf->f_bfree = jittered_free / buf->f_frsize;
        buf->f_bavail = buf->f_bfree;
        LOGI("Puppy spoofed statvfs64 for path: %s", path);
    }
    return ret;
}
#endif

#if defined(SPECS_AVAIL_RAM) && defined(SPECS_TOTAL_RAM)
static int hook_sysinfo(struct sysinfo *info) {
    int ret = orig_sysinfo(info);
    if (ret == 0 && info != NULL) {
        long long jittered_avail_ram = get_realistic_jitter(SPECS_AVAIL_RAM, 0.03);
        info->totalram = SPECS_TOTAL_RAM / info->mem_unit;
        info->freeram = jittered_avail_ram / info->mem_unit;
        LOGI("Puppy spoofed sysinfo (RAM)");
    }
    return ret;
}
#endif

static void* puppy_main_thread(void* arg) {
    sleep(3);
    
    LOGI("Puppy waking up in the background... sniffing out environment.");
    gum_init_embedded();

    GumInterceptor *interceptor = gum_interceptor_obtain();
    if (interceptor == NULL) {
        LOGE("Puppy lost the scent: failed to obtain GumInterceptor.");
        return NULL;
    }

    gum_interceptor_begin_transaction(interceptor);

    // 1. System Property Hook
    #ifdef SYSTEM_PROP_OVERRIDES_COUNT
    gpointer target_prop = GSIZE_TO_POINTER(dlsym(RTLD_DEFAULT, "__system_property_get"));
    if (target_prop) {
        gum_interceptor_replace(
            interceptor,
            target_prop, 
            GSIZE_TO_POINTER(hook_system_property_get),
            (gpointer *)&orig_system_property_get,
            NULL
        );
    }
    #endif

    // 2. Display Metrics Hook
    #if defined(SPECS_WIDTH) || defined(SPECS_HEIGHT)
    void *libnativewindow = dlopen("libnativewindow.so", RTLD_LAZY);
    #endif

    #ifdef SPECS_WIDTH
    gpointer target_width = libnativewindow ? dlsym(libnativewindow, "ANativeWindow_getWidth") : NULL;
    if (target_width) {
        gum_interceptor_replace(
            interceptor,
            target_width, 
            GSIZE_TO_POINTER(hook_anativewindow_get_width),
            (gpointer *)&orig_anativewindow_get_width,
            NULL
        );
    }
    #endif

    #ifdef SPECS_HEIGHT
    gpointer target_height = libnativewindow ? dlsym(libnativewindow, "ANativeWindow_getHeight") : NULL;
    if (target_height) {
        gum_interceptor_replace(
            interceptor,
            target_height, 
            GSIZE_TO_POINTER(hook_anativewindow_get_height),
            (gpointer *)&orig_anativewindow_get_height,
            NULL
        );
    }
    #endif

    // 3. Storage & Memory Hooks
    #if defined(SPECS_FREE_SPACE) && defined(SPECS_TOTAL_SPACE)
    gpointer target_statfs = GSIZE_TO_POINTER(dlsym(RTLD_DEFAULT, "statfs"));
    gpointer target_statfs64  = GSIZE_TO_POINTER(dlsym(RTLD_DEFAULT, "statfs64"));
    gpointer target_statvfs = GSIZE_TO_POINTER(dlsym(RTLD_DEFAULT, "statvfs"));
    gpointer target_statvfs64 = GSIZE_TO_POINTER(dlsym(RTLD_DEFAULT, "statvfs64"));
    if (target_statfs) {
        gum_interceptor_replace(interceptor, target_statfs, GSIZE_TO_POINTER(hook_statfs), (gpointer *)&orig_statfs, NULL);
    }
    if (target_statfs64) {
        gum_interceptor_replace(interceptor, target_statfs64, GSIZE_TO_POINTER(hook_statfs64), (gpointer *)&orig_statfs64, NULL);
    }
    if (target_statvfs) {
        gum_interceptor_replace(interceptor, target_statvfs, GSIZE_TO_POINTER(hook_statvfs), (gpointer *)&orig_statvfs, NULL);
    }
    if (target_statvfs64) {
        gum_interceptor_replace(interceptor, target_statvfs64, GSIZE_TO_POINTER(hook_statvfs64), (gpointer *)&orig_statvfs64, NULL);
    }
    #endif

    #if defined(SPECS_AVAIL_RAM) && defined(SPECS_TOTAL_RAM)
    gpointer target_sysinfo = GSIZE_TO_POINTER(dlsym(RTLD_DEFAULT, "sysinfo"));
    if (target_sysinfo) {
        gum_interceptor_replace(interceptor, target_sysinfo, GSIZE_TO_POINTER(hook_sysinfo), (gpointer *)&orig_sysinfo, NULL);
    }
    #endif

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