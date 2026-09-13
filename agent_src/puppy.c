#include "frida-gum.h"
#include "config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <dlfcn.h>
#include <math.h>
#include <sys/vfs.h>
#include <sys/statvfs.h>
#include <sys/sysinfo.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/system_properties.h>

#ifndef __NR_memfd_create
#if defined(__arm__)
#define __NR_memfd_create 385
#elif defined(__aarch64__)
#define __NR_memfd_create 279
#elif defined(__i386__)
#define __NR_memfd_create 356
#elif defined(__x86_64__)
#define __NR_memfd_create 319
#endif
#endif

#ifndef MFD_CLOEXEC
#define MFD_CLOEXEC 0x0001U
#endif

#include <android/log.h>
#include <android/native_window.h>

#define LOG_TAG "[Puppy]"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

typedef int (*SystemPropertyGetFunc)(const char *name, char *value);
typedef void (*PropReadCallback)(void *cookie, const char *name, const char *value, uint32_t serial);
typedef void (*SystemPropertyReadCallbackFunc)(const prop_info *pi, PropReadCallback callback, void *cookie);

typedef int32_t (*ANativeWindowGetWidthFunc)(ANativeWindow *window);
typedef int32_t (*ANativeWindowGetHeightFunc)(ANativeWindow *window);

typedef int (*StatfsFunc)(const char *path, struct statfs *buf);
typedef int (*Statfs64Func)(const char *path, struct statfs64 *buf);
typedef int (*StatvfsFunc)(const char *path, struct statvfs *buf);
typedef int (*Statvfs64Func)(const char *path, struct statvfs64 *buf);
typedef int (*SysinfoFunc)(struct sysinfo *info);

typedef int (*ExecveFunc)(const char *pathname, char *const argv[], char *const envp[]);

typedef int (*OpenatFunc)(int dirfd, const char *pathname, int flags, int mode);

static SystemPropertyGetFunc orig_system_property_get = NULL;
static SystemPropertyReadCallbackFunc orig_system_property_read_callback = NULL;

static ANativeWindowGetWidthFunc orig_anativewindow_get_width = NULL;
static ANativeWindowGetHeightFunc orig_anativewindow_get_height = NULL;

static StatfsFunc orig_statfs = NULL;
static Statfs64Func orig_statfs64 = NULL;
static StatvfsFunc orig_statvfs = NULL;
static Statvfs64Func orig_statvfs64 = NULL;
static SysinfoFunc orig_sysinfo = NULL;

static ExecveFunc orig_execve = NULL;

static OpenatFunc orig_openat = NULL;

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

typedef struct {
    PropReadCallback orig_callback;
    void *orig_cookie;
} PropReadContext;

#ifdef SYSTEM_PROP_OVERRIDES_COUNT
static void puppy_prop_read_wrapper(void *cookie, const char *name, const char *value, uint32_t serial) {
    PropReadContext *ctx = (PropReadContext *)cookie;
    if (ctx == NULL) return;

    const char *final_value = value;
    #ifdef SYSTEM_PROP_OVERRIDES_COUNT
    if (name != NULL) {
        for (size_t i = 0; i < SYSTEM_PROP_OVERRIDES_COUNT; i++) {
            if (strcmp(name, SYSTEM_PROP_OVERRIDES[i].key) == 0) {
                final_value = SYSTEM_PROP_OVERRIDES[i].value;
                break;
            }
        }
    }
    #endif

    ctx->orig_callback(ctx->orig_cookie, name, final_value, serial);
}

static void hook_system_property_read_callback(const prop_info *pi, PropReadCallback callback, void *cookie) {
    if (pi != NULL && callback != NULL) {
        PropReadContext ctx;
        ctx.orig_callback = callback;
        ctx.orig_cookie = cookie;
        orig_system_property_read_callback(pi, puppy_prop_read_wrapper, &ctx);
        return;
    }
    orig_system_property_read_callback(pi, callback, cookie);
}

static int hook_execve(const char *pathname, char *const argv[], char *const envp[]) {
    if (pathname != NULL) {
        #ifdef SYSTEM_PROP_OVERRIDES_COUNT
        if (strstr(pathname, "sh") != NULL && argv != NULL) {
            for (int j = 1; argv[j] != NULL; j++) {
                if (strcmp(argv[j], "-c") == 0 && argv[j + 1] != NULL) {
                    char *cmd = argv[j + 1];
                    if (strstr(cmd, "getprop") != NULL) {
                        for (size_t i = 0; i < SYSTEM_PROP_OVERRIDES_COUNT; i++) {
                            if (strstr(cmd, SYSTEM_PROP_OVERRIDES[i].key) != NULL) {
                                LOGI("Puppy intercepted shell getprop for: %s. Redirecting via echo.", SYSTEM_PROP_OVERRIDES[i].key);
                                char *new_argv[4];
                                new_argv[0] = (char *)"/system/bin/sh";
                                new_argv[1] = (char *)"-c";
                                int len = snprintf(NULL, 0, "echo %s", SYSTEM_PROP_OVERRIDES[i].value) + 1;
                                char *echo_cmd = (char *)malloc(len);
                                snprintf(echo_cmd, len, "echo %s", SYSTEM_PROP_OVERRIDES[i].value);
                                new_argv[2] = echo_cmd;
                                new_argv[3] = NULL;
                                
                                int ret = orig_execve("/system/bin/sh", new_argv, envp);
                                free(echo_cmd);
                                return ret;
                            }
                        }
                    }
                }
            }
        }
        #endif
    }
    return orig_execve(pathname, argv, envp);
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

#if defined(DEVICE_CPU_INFO) || (defined(SPECS_TOTAL_RAM) && defined(SPECS_AVAIL_RAM) && defined(SPECS_SWAP_TOTAL_KB))
static int get_intercepted_fd(const char *pathname);

static int hook_openat(int dirfd, const char *pathname, int flags, int mode) {
    int fake_fd = get_intercepted_fd(pathname);
    if (fake_fd >= 0) {
        return fake_fd;
    }
    return orig_openat(dirfd, pathname, flags, mode);
}

static int create_memfd_with_data(const char *name, const char *data, size_t size) {
    int fd = (int) syscall(__NR_memfd_create, name, MFD_CLOEXEC);
    if (fd < 0) {
        return -1;
    }
    if (ftruncate(fd, (off_t)size) < 0) {
        close(fd);
        return -1;
    }
    ssize_t written = write(fd, data, size);
    if (written < 0) {
        close(fd);
        return -1;
    }
    lseek(fd, 0, SEEK_SET);
    return fd;
}

#if defined(SPECS_TOTAL_RAM) && defined(SPECS_AVAIL_RAM) && defined(SPECS_SWAP_TOTAL_KB)
static unsigned long long apply_jitter(unsigned long long base_value, double variance_percent) {
    double min = base_value * (1.0 - variance_percent);
    double max = base_value * (1.0 + variance_percent);
    double range = max - min;
    if (range < 0) range = 0;
    double random_val = min + ((double)rand() / RAND_MAX) * range;
    return (unsigned long long)(random_val + 0.5);
}
#endif

static int get_intercepted_fd(const char *pathname) {
    if (pathname == NULL) return -1;

    #ifdef DEVICE_CPU_INFO
    if (strcmp(pathname, "/proc/cpuinfo") == 0) {
        LOGI("Puppy intercepted open for /proc/cpuinfo");
        
        return create_memfd_with_data("cpuinfo_fake", DEVICE_CPU_INFO, strlen(DEVICE_CPU_INFO));
        
    }
    #endif
    
    if (strcmp(pathname, "/sys/fs/selinux/enforce") == 0) {
        LOGI("Puppy intercepted open for /sys/fs/selinux/enforce");
        const char *val = "1\n";
        return create_memfd_with_data("selinux_fake", val, strlen(val));
    }
    
    #if defined(SPECS_TOTAL_RAM) && defined(SPECS_AVAIL_RAM) && defined(SPECS_SWAP_TOTAL_KB)
    if (strcmp(pathname, "/proc/meminfo") == 0) {
        LOGI("Puppy intercepted open for /proc/meminfo");
        
        unsigned long long _MEM_TOTAL_KB = SPECS_TOTAL_RAM / 1024;
        unsigned long long _SWAP_TOTAL_KB = SPECS_SWAP_TOTAL_KB;
        unsigned long long _VMALLOC_TOTAL = 262143999ULL;
        
        unsigned long long base_avail = SPECS_AVAIL_RAM / 1024;
        unsigned long long _MEM_AVAIL_KB = apply_jitter(base_avail, 0.02);
        unsigned long long _MEM_FREE_KB = apply_jitter((unsigned long long)(_MEM_AVAIL_KB * 0.35), 0.05);
        unsigned long long _CACHED_KB = apply_jitter((unsigned long long)(_MEM_AVAIL_KB * 0.60), 0.03);
        
        unsigned long long _ACTIVE = apply_jitter(3256780, 0.04);
        unsigned long long _INACTIVE = apply_jitter(2564120, 0.04);
        unsigned long long _ANON_PAGES = apply_jitter(2846100, 0.02);
        unsigned long long _MAPPED = apply_jitter(856000, 0.03);
        unsigned long long _SLAB = apply_jitter(785600, 0.01);
        unsigned long long _DIRTY = (unsigned long long)(rand() % 800);

        char meminfo_buf[2048];
        int len = snprintf(meminfo_buf, sizeof(meminfo_buf),
            "MemTotal:        %llu kB\n"
            "MemFree:         %llu kB\n"
            "MemAvailable:    %llu kB\n"
            "Buffers:           32768 kB\n"
            "Cached:          %llu kB\n"
            "SwapCached:        55020 kB\n"
            "Active:          %llu kB\n"
            "Inactive:        %llu kB\n"
            "Active(anon):    %llu kB\n"
            "Inactive(anon):  %llu kB\n"
            "Active(file):    %llu kB\n"
            "Inactive(file):  %llu kB\n"
            "Unevictable:      186400 kB\n"
            "Mlocked:          186400 kB\n"
            "SwapTotal:       %llu kB\n"
            "SwapFree:        %llu kB\n"
            "Zswap:           %llu kB\n"
            "Zswapped:        %llu kB\n"
            "Dirty:               %llu kB\n"
            "Writeback:             0 kB\n"
            "AnonPages:       %llu kB\n"
            "Mapped:          %llu kB\n"
            "Shmem:             52500 kB\n"
            "KReclaimable:     754200 kB\n"
            "Slab:             %llu kB\n"
            "SReclaimable:     %llu kB\n"
            "SUnreclaim:       %llu kB\n"
            "KernelStack:       %llu kB\n"
            "PageTables:       %llu kB\n"
            "SecPageTables:         0 kB\n"
            "NFS_Unstable:          0 kB\n"
            "Bounce:                0 kB\n"
            "WritebackTmp:          0 kB\n"
            "CommitLimit:    16260632 kB\n"
            "Committed_AS:   95421000 kB\n"
            "VmallocTotal:   %llu kB\n"
            "VmallocUsed:      %llu kB\n"
            "VmallocChunk:          0 kB\n"
            "Percpu:            17200 kB\n"
            "HardwareCorrupted:     0 kB\n"
            "AnonHugePages:    640672 kB\n"
            "ShmemHugePages:        0 kB\n"
            "ShmemPmdMapped:        0 kB\n"
            "FileHugePages:         0 kB\n"
            "FilePmdMapped:         0 kB\n"
            "CmaTotal:         512000 kB\n"
            "CmaFree:               0 kB\n",
            _MEM_TOTAL_KB, _MEM_FREE_KB, _MEM_AVAIL_KB, _CACHED_KB,
            _ACTIVE, _INACTIVE,
            apply_jitter(2890100, 0.04), apply_jitter(745600, 0.04),
            apply_jitter(1166680, 0.04), apply_jitter(2718520, 0.04),
            _SWAP_TOTAL_KB, apply_jitter(6085412, 0.05),
            apply_jitter(1450000, 0.02), apply_jitter(1254300, 0.02),
            _DIRTY, _ANON_PAGES, _MAPPED, _SLAB,
            apply_jitter(554200, 0.01), apply_jitter(331400, 0.01),
            apply_jitter(75400, 0.05), apply_jitter(285200, 0.02),
            _VMALLOC_TOTAL, apply_jitter(385400, 0.01)
        );
        
        return create_memfd_with_data("meminfo_fake", meminfo_buf, len);
    }
    #endif

    return -1;
}
#endif

static void* puppy_main_thread(void* arg) {
    LOGI("Puppy waking up in the background... sniffing out environment.");
    gum_init_embedded();

    GumInterceptor *interceptor = gum_interceptor_obtain();
    if (interceptor == NULL) {
        LOGE("Puppy lost the scent: failed to obtain GumInterceptor.");
        return NULL;
    }

    gum_interceptor_begin_transaction(interceptor);

    // 1. System Property Hooks
    #ifdef SYSTEM_PROP_OVERRIDES_COUNT
    gpointer target_prop_get = GSIZE_TO_POINTER(dlsym(RTLD_DEFAULT, "__system_property_get"));
    if (target_prop_get) {
        gum_interceptor_replace(
            interceptor,
            target_prop_get, 
            GSIZE_TO_POINTER(hook_system_property_get),
            (gpointer *)&orig_system_property_get,
            NULL
        );
    }
    gpointer target_prop_read_cb = GSIZE_TO_POINTER(dlsym(RTLD_DEFAULT, "__system_property_read_callback"));
    if (target_prop_read_cb) {
        gum_interceptor_replace(
            interceptor,
            target_prop_read_cb, 
            GSIZE_TO_POINTER(hook_system_property_read_callback),
            (gpointer *)&orig_system_property_read_callback,
            NULL
        );
    }
    gpointer target_execve = GSIZE_TO_POINTER(dlsym(RTLD_DEFAULT, "execve"));
    if (target_execve) {
        gum_interceptor_replace(
            interceptor,
            target_execve, 
            GSIZE_TO_POINTER(hook_execve),
            (gpointer *)&orig_execve,
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

    // 5. File Open Hook (openat) for meminfo, cpuinfo, selinux
    #if defined(DEVICE_CPU_INFO) || (defined(SPECS_TOTAL_RAM) && defined(SPECS_AVAIL_RAM) && defined(SPECS_SWAP_TOTAL_KB))
    gpointer target_openat = GSIZE_TO_POINTER(dlsym(RTLD_DEFAULT, "openat"));
    if (target_openat) {
        gum_interceptor_replace(
            interceptor,
            target_openat, 
            GSIZE_TO_POINTER(hook_openat),
            (gpointer *)&orig_openat,
            NULL
        );
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