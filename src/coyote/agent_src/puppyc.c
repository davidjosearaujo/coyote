#include "frida-gumjs.h"
#include "config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <dlfcn.h>
#include <spawn.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <sys/prctl.h>
#include <dirent.h>

#include <sys/vfs.h>
#include <sys/statvfs.h>
#include <sys/sysinfo.h>
#include <sys/syscall.h>
#include <sys/system_properties.h>
#include <time.h>
#include <errno.h>

#include <android/log.h>
#include <android/native_window.h>

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

// The deployed library's basename. The build randomizes it (-DCOYOTE_SO_NAME)
// so there is no fixed "libcoyote.so" signature on disk / in /proc; the native
// hiding (maps, stat/access, readdir) keys off this exact name.
#ifndef COYOTE_SO_NAME
#define COYOTE_SO_NAME "libcoyote.so"
#endif

#define LOG_TAG "[PuppyC]"
#ifdef COYOTE_DEBUG
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)
#else
#define LOGI(...) ((void)0)
#define LOGE(...) ((void)0)
#endif

// Recon logging is independent of debug/release: in recon builds it records the
// probes the target makes (under its own tag) regardless of COYOTE_DEBUG.
#ifdef COYOTE_RECON
#define RLOG(...) __android_log_print(ANDROID_LOG_INFO, "[CoyoteRecon]", __VA_ARGS__)
#else
#define RLOG(...) ((void)0)
#endif

typedef int (*SystemPropertyGetFunc)(const char *name, char *value);
typedef void (*PropReadCallback)(void *cookie, const char *name, const char *value, uint32_t serial);
typedef void (*SystemPropertyReadCallbackFunc)(const prop_info *pi, PropReadCallback callback, void *cookie);
typedef const prop_info *(*SystemPropertyFindFunc)(const char *name);

typedef int32_t (*ANativeWindowGetWidthFunc)(ANativeWindow *window);
typedef int32_t (*ANativeWindowGetHeightFunc)(ANativeWindow *window);

typedef int (*StatfsFunc)(const char *path, struct statfs *buf);
typedef int (*Statfs64Func)(const char *path, struct statfs64 *buf);
typedef int (*StatvfsFunc)(const char *path, struct statvfs *buf);
typedef int (*Statvfs64Func)(const char *path, struct statvfs64 *buf);
typedef int (*SysinfoFunc)(struct sysinfo *info);

typedef int (*PosixSpawnFunc)(pid_t *pid, const char *path,
                             const posix_spawn_file_actions_t *file_actions,
                             const posix_spawnattr_t *attrp,
                             char *const argv[], char *const envp[]);
typedef int (*PosixSpawnpFunc)(pid_t *pid, const char *path,
                              const posix_spawn_file_actions_t *file_actions,
                              const posix_spawnattr_t *attrp,
                              char *const argv[], char *const envp[]);
typedef int (*ExecveFunc)(const char *path, char *const argv[], char *const envp[]);
typedef int (*ExecvpeFunc)(const char *path, char *const argv[], char *const envp[]);

typedef int (*OpenatFunc)(int dirfd, const char *path, int flags, int mode);
typedef int (*OpenFunc)(const char *path, int flags, int mode);

typedef const char *(*GlGetStringFunc)(unsigned int name);

typedef void (*PFN_vkVoidFunction)(void);
typedef PFN_vkVoidFunction (*VkGetInstanceProcAddrFunc)(void* instance, const char* pName);
typedef void (*VkGetPhysicalDevicePropertiesFunc)(void* physicalDevice, void* pProperties);

static SystemPropertyGetFunc orig_system_property_get = NULL;
static SystemPropertyReadCallbackFunc orig_system_property_read_callback = NULL;
static SystemPropertyFindFunc orig_system_property_find = NULL;

static ANativeWindowGetWidthFunc orig_anativewindow_get_width = NULL;
static ANativeWindowGetHeightFunc orig_anativewindow_get_height = NULL;

static StatfsFunc orig_statfs = NULL;
static Statfs64Func orig_statfs64 = NULL;
static StatvfsFunc orig_statvfs = NULL;
static Statvfs64Func orig_statvfs64 = NULL;

static PosixSpawnFunc orig_posix_spawn = NULL;
static PosixSpawnpFunc orig_posix_spawnp = NULL;
static ExecveFunc orig_execve = NULL;
static ExecvpeFunc orig_execvpe = NULL;

static OpenatFunc orig_openat = NULL;
static OpenFunc orig_open = NULL;

typedef long (*PtraceFunc)(int request, pid_t pid, void *addr, void *data);
static PtraceFunc orig_ptrace = NULL;

static GlGetStringFunc orig_glGetString = NULL;
#define GL_VENDOR   0x1F00
#define GL_RENDERER 0x1F01
#define GL_VERSION  0x1F02

static VkGetInstanceProcAddrFunc orig_vkGetInstanceProcAddr = NULL;
static VkGetPhysicalDevicePropertiesFunc real_vkGetPhysicalDeviceProperties = NULL;

static char puppy_so_path[PATH_MAX] = {0};

static char **build_preloaded_envp(char *const envp[]) {
    if (puppy_so_path[0] == '\0') return (char **)envp;

    size_t count = 0;
    int has_ld_preload = 0;

    if (envp) {
        while (envp[count] != NULL) {
            if (strncmp(envp[count], "LD_PRELOAD=", 11) == 0) {
                has_ld_preload = 1;
            }
            count++;
        }
    }

    char **new_envp = (char **)malloc(sizeof(char *) * (count + 2));
    if (!new_envp) return (char **)envp;

    size_t idx = 0;
    if (envp) {
        for (size_t i = 0; i < count; i++) {
            if (strncmp(envp[i], "LD_PRELOAD=", 11) == 0) {
                size_t len = strlen(envp[i]) + strlen(puppy_so_path) + 2;
                char *buf = (char *)malloc(len);
                snprintf(buf, len, "%s:%s", envp[i], puppy_so_path);
                new_envp[idx++] = buf;
            } else {
                new_envp[idx++] = strdup(envp[i]);
            }
        }
    }

    if (!has_ld_preload) {
        size_t len = strlen(puppy_so_path) + 12;
        char *buf = (char *)malloc(len);
        snprintf(buf, len, "LD_PRELOAD=%s", puppy_so_path);
        new_envp[idx++] = buf;
    }

    new_envp[idx] = NULL;
    return new_envp;
}

static void free_preloaded_envp(char **new_envp, char *const orig_envp[]) {
    if (new_envp == (char **)orig_envp || !new_envp) return;
    for (size_t i = 0; new_envp[i] != NULL; i++) {
        free(new_envp[i]);
    }
    free(new_envp);
}

#ifdef SYSTEM_PROP_OVERRIDES_COUNT
static int prop_override_cmp(const void *key, const void *elem) {
    return strcmp((const char *)key, ((const prop_override_t *)elem)->key);
}

// SYSTEM_PROP_OVERRIDES is emitted sorted by key (see cli.py), so this is an
// O(log n) binary search instead of a linear strcmp scan. __system_property_get
// is called constantly during an app's life, so this is the one genuinely hot
// native hook worth optimizing.
static const char* get_mocked_prop_value(const char *key) {
    if (!key) return NULL;
    const prop_override_t *hit = (const prop_override_t *)bsearch(
        key, SYSTEM_PROP_OVERRIDES, SYSTEM_PROP_OVERRIDES_COUNT,
        sizeof(SYSTEM_PROP_OVERRIDES[0]), prop_override_cmp);
    return hit ? hit->value : NULL;
}

static int hook_system_property_get(const char *name, char *value) {
    if (name != NULL && value != NULL) {
        const char *mock_val = get_mocked_prop_value(name);
        RLOG("prop_get %s%s", name, mock_val != NULL ? " [spoofed]" : "");
        if (mock_val != NULL) {
            strlcpy(value, mock_val, PROP_VALUE_MAX);
            return (int)strlen(value);
        }
    }
    return orig_system_property_get(name, value);
}

typedef struct {
    PropReadCallback orig_callback;
    void *orig_cookie;
} PropReadContext;

static void prop_read_wrapper(void *cookie, const char *name, const char *value, uint32_t serial) {
    PropReadContext *ctx = (PropReadContext *)cookie;
    if (ctx == NULL) return;

    const char *final_value = value;
    if (name != NULL) {
        const char *mock_val = get_mocked_prop_value(name);
        if (mock_val != NULL) {
            final_value = mock_val;
        }
    }

    ctx->orig_callback(ctx->orig_cookie, name, final_value, serial);
}

static void hook_system_property_read_callback(const prop_info *pi, PropReadCallback callback, void *cookie) {
    uintptr_t pi_addr = (uintptr_t)pi;
    uintptr_t start_addr = (uintptr_t)&SYSTEM_PROP_OVERRIDES[0];
    uintptr_t end_addr = start_addr + sizeof(SYSTEM_PROP_OVERRIDES);

    if (pi_addr >= start_addr && pi_addr < end_addr) {
        const prop_override_t *entry = (const prop_override_t *)pi;
        if (callback != NULL && entry->key != NULL && entry->value != NULL) {
            callback(cookie, entry->key, entry->value, 0);
        }
        return;
    }

    if (pi != NULL && callback != NULL) {
        PropReadContext ctx;
        ctx.orig_callback = callback;
        ctx.orig_cookie = cookie;
        orig_system_property_read_callback(pi, prop_read_wrapper, &ctx);
        return;
    }
    orig_system_property_read_callback(pi, callback, cookie);
}

static const prop_info *hook_system_property_find(const char *name) {
    if (name != NULL && get_mocked_prop_value(name) != NULL) {
        const prop_info *real_pi = orig_system_property_find(name);
        if (real_pi != NULL) {
            return real_pi;
        }
        for (size_t i = 0; i < SYSTEM_PROP_OVERRIDES_COUNT; i++) {
            if (strcmp(name, SYSTEM_PROP_OVERRIDES[i].key) == 0) {
                return (const prop_info *)&SYSTEM_PROP_OVERRIDES[i];
            }
        }
    }
    return orig_system_property_find(name);
}

static int is_root_check_cmd(const char *path, char *const argv[]) {
    const char *bad_commands[] = {"su", "magisk", "frida", "xposed", "root", "busybox", NULL};
    
    if (path) {
        for (int i = 0; bad_commands[i] != NULL; i++) {
            if (strstr(path, bad_commands[i]) != NULL) return 1;
        }
    }
    
    if (argv) {
        for (int i = 0; argv[i] != NULL; i++) {
            for (int j = 0; bad_commands[j] != NULL; j++) {
                if (strstr(argv[i], bad_commands[j]) != NULL) return 1;
            }
        }
    }
    
    return 0;
}

// Paths whose mere existence reveals root/Magisk/instrumentation. Root detection
// overwhelmingly works by probing these with File.exists()/access()/stat(), so
// the file-op hooks below report ENOENT for them. The su binary is matched by
// full known paths (not a bare "su" substring) to avoid false positives; the
// distinctive tokens are safe as substrings.
static int is_hidden_path(const char *path) {
    if (!path) return 0;
    static const char *hidden[] = {
        "/system/xbin/su", "/system/bin/su", "/sbin/su", "/su/bin/su",
        "/vendor/bin/su", "/system/sbin/su", "/data/local/bin/su",
        "/data/local/xbin/su", "/data/local/su", "/magisk/.core/bin/su",
        "/system/app/Superuser", "/system/app/SuperSU",
        "/sbin/.magisk", "/data/adb/magisk", "/data/adb/modules",
        "/data/adb/ksu", "/dev/.magisk", "/cache/.disable_magisk",
        "/init.magisk.rc", "/system/etc/init/magisk",
        "magisk", "busybox", "superuser", "frida", "xposed", "/re.frida",
        // The native-bridge shim (experimental path) lives in the app data dir;
        // safe to hide from stat/access since nothing in-process loads it.
        // NOTE: do NOT hide the injected library (COYOTE_SO_NAME) here — the
        // dynamic linker/ART legitimately stat()s the mapped .so, and returning
        // ENOENT for it breaks Java-bridge hook installation. Its on-disk
        // signature is handled by randomizing the name; its memory footprint by
        // the /proc/.../maps filter below.
        "coyote_nb.sh",
        NULL
    };
    for (int i = 0; hidden[i] != NULL; i++) {
        if (strstr(path, hidden[i]) != NULL) return 1;
    }
    return 0;
}

static int is_getprop_cmd(const char *path, char *const argv[], const char **out_prop_name) {
    if (!path || !argv || !argv[0]) return 0;

    const char *bin = strrchr(path, '/');
    bin = bin ? bin + 1 : path;

    if (strcmp(bin, "getprop") == 0 && argv[1] != NULL) {
        char *key = argv[1];
        char *delim = strpbrk(key, " \"'\t\r\n>|&;");
        if (delim) *delim = '\0';
        
        *out_prop_name = key;
        return 1;
    }

    // Shell invocation: sh -c "getprop <key>" or su -c "getprop <key>"
    if ((strcmp(bin, "sh") == 0 || strcmp(bin, "su") == 0) && argv[1] && strcmp(argv[1], "-c") == 0 && argv[2]) {
        if (strncmp(argv[2], "getprop ", 8) == 0) {
            char *key = argv[2] + 8;
            
            while (*key == ' ' || *key == '"' || *key == '\'') key++;
            
            char *delim = strpbrk(key, " \"'\t\r\n>|&;");
            if (delim) *delim = '\0';
            
            *out_prop_name = key;
            return 1;
        }
    }

    return 0;
}

static int hook_posix_spawn(pid_t *pid, const char *path,
                           const posix_spawn_file_actions_t *file_actions,
                           const posix_spawnattr_t *attrp,
                           char *const argv[], char *const envp[]) {
    if (is_root_check_cmd(path, argv)) {
        LOGI("PuppyC blocked root check (posix_spawn): %s", path);
        char *fake_argv[] = {"/system/bin/echo", NULL};
        return orig_posix_spawn(pid, "/system/bin/echo", file_actions, attrp, fake_argv, envp);
    }
    const char *prop_name = NULL;
    if (is_getprop_cmd(path, argv, &prop_name)) {
        const char *mock_val = get_mocked_prop_value(prop_name);
        if (mock_val) {
            // Exec `echo <val>` directly (argv, not a shell) so a value with a
            // quote or shell metacharacter needs no escaping and can't inject.
            char *fake_argv[] = {"echo", (char *) mock_val, NULL};
            return orig_posix_spawn(pid, "/system/bin/echo", file_actions, attrp, fake_argv, envp);
        }
    }

    char **new_envp = build_preloaded_envp(envp);
    int res = orig_posix_spawn(pid, path, file_actions, attrp, argv, new_envp);
    free_preloaded_envp(new_envp, envp);
    return res;
}

static int hook_posix_spawnp(pid_t *pid, const char *path,
                            const posix_spawn_file_actions_t *file_actions,
                            const posix_spawnattr_t *attrp,
                            char *const argv[], char *const envp[]) {
    if (is_root_check_cmd(path, argv)) {
        LOGI("PuppyC blocked root check (posix_spawn): %s", path);
        char *fake_argv[] = {"echo", NULL};
        return orig_posix_spawnp(pid, "echo", file_actions, attrp, fake_argv, envp);
    }
    const char *prop_name = NULL;
    if (is_getprop_cmd(path, argv, &prop_name)) {
        const char *mock_val = get_mocked_prop_value(prop_name);
        if (mock_val) {
            // Exec `echo <val>` directly (argv, not a shell), no quoting needed.
            char *fake_argv[] = {"echo", (char *) mock_val, NULL};
            return orig_posix_spawnp(pid, "/system/bin/echo", file_actions, attrp, fake_argv, envp);
        }
    }

    char **new_envp = build_preloaded_envp(envp);
    int res = orig_posix_spawnp(pid, path, file_actions, attrp, argv, new_envp);
    free_preloaded_envp(new_envp, envp);
    return res;
}

static int hook_execve(const char *path, char *const argv[], char *const envp[]) {
    if (is_root_check_cmd(path, argv)) {
        LOGI("PuppyC blocked root check (execve): %s", path);
        char *fake_argv[] = {"/system/bin/echo", NULL};
        return orig_execve("/system/bin/echo", fake_argv, envp);
    }
    const char *prop_name = NULL;
    if (is_getprop_cmd(path, argv, &prop_name)) {
        const char *mock_val = get_mocked_prop_value(prop_name);
        if (mock_val) {
            char *fake_argv[] = {"echo", (char *)mock_val, NULL};
            return orig_execve("/system/bin/echo", fake_argv, envp);
        }
    }

    char **new_envp = build_preloaded_envp(envp);
    int res = orig_execve(path, argv, new_envp);
    free_preloaded_envp(new_envp, envp);
    return res;
}

static int hook_execv(const char *path, char *const argv[]) {
    if (is_root_check_cmd(path, argv)) {
        LOGI("PuppyC blocked root check (execv): %s", path);
        char *fake_argv[] = {"/system/bin/echo", NULL};
        char **new_envp = build_preloaded_envp(NULL);
        int res = orig_execve("/system/bin/echo", fake_argv, new_envp);
        free_preloaded_envp(new_envp, NULL);
        return res;
    }
    const char *prop_name = NULL;
    if (is_getprop_cmd(path, argv, &prop_name)) {
        const char *mock_val = get_mocked_prop_value(prop_name);
        if (mock_val) {
            char *fake_argv[] = {"echo", (char *)mock_val, NULL};
            return orig_execve("/system/bin/echo", fake_argv, environ);
        }
    }

    char **new_envp = build_preloaded_envp(NULL);
    int res = orig_execve(path, argv, new_envp);
    free_preloaded_envp(new_envp, NULL);
    return res;
}

static int hook_execvp(const char *path, char *const argv[]) {
    if (is_root_check_cmd(path, argv)) {
        LOGI("PuppyC blocked root check (execvp): %s", path);
        char *fake_argv[] = {"echo", NULL};
        char **new_envp = build_preloaded_envp(NULL);
        int res = orig_execvpe("echo", fake_argv, new_envp);
        free_preloaded_envp(new_envp, NULL);
        return res;
    }
    const char *prop_name = NULL;
    if (is_getprop_cmd(path, argv, &prop_name)) {
        const char *mock_val = get_mocked_prop_value(prop_name);
        if (mock_val) {
            char *fake_argv[] = {"echo", (char *)mock_val, NULL};
            return orig_execve("/system/bin/echo", fake_argv, environ);
        }
    }

    char **new_envp = build_preloaded_envp(NULL);
    int res = orig_execvpe(path, argv, new_envp);
    free_preloaded_envp(new_envp, NULL);
    return res;
}

static int hook_execvpe(const char *path, char *const argv[], char *const envp[]) {
    if (is_root_check_cmd(path, argv)) {
        LOGI("PuppyC blocked root check (execvp): %s", path);
        char *fake_argv[] = {"echo", NULL};
        char **new_envp = build_preloaded_envp(NULL);
        int res = orig_execvpe("echo", fake_argv, new_envp);
        free_preloaded_envp(new_envp, NULL);
        return res;
    }
    const char *prop_name = NULL;
    if (is_getprop_cmd(path, argv, &prop_name)) {
        const char *mock_val = get_mocked_prop_value(prop_name);
        if (mock_val) {
            char *fake_argv[] = {"echo", (char *)mock_val, NULL};
            return orig_execve("/system/bin/echo", fake_argv, envp);
        }
    }

    char **new_envp = build_preloaded_envp(envp);
    int res = orig_execvpe(path, argv, new_envp);
    free_preloaded_envp(new_envp, envp);
    return res;
}
#endif

#ifdef SPECS_WIDTH
static int32_t hook_anativewindow_get_width(ANativeWindow *window) {
    LOGI("PuppyC intercepted ANativeWindow_getWidth! Spoofing width.");
    return SPECS_WIDTH;
}
#endif

#ifdef SPECS_HEIGHT
static int32_t hook_anativewindow_get_height(ANativeWindow *window) {
    LOGI("PuppyC intercepted ANativeWindow_getHeight! Spoofing height.");
    return SPECS_HEIGHT;
}
#endif

static unsigned long long apply_jitter(unsigned long long base_val, double percentage) {
    double fluctuation = ((double)rand() / RAND_MAX) * 2.0 - 1.0; 
    double jittered = (double)base_val + ((double)base_val * percentage * fluctuation);
    return (jittered < 0.0) ? 0ULL : (unsigned long long)(jittered + 0.5);
}

#if defined(SPECS_FREE_SPACE) && defined(SPECS_TOTAL_SPACE)
static int hook_statfs(const char *path, struct statfs *buf) {
    int ret = orig_statfs(path, buf);
    if (ret == 0 && buf != NULL) {
        long long jittered_free = apply_jitter(SPECS_FREE_SPACE, 0.0005);
        buf->f_blocks = SPECS_TOTAL_SPACE / buf->f_bsize;
        buf->f_bfree = jittered_free / buf->f_bsize;
        buf->f_bavail = buf->f_bfree; 
        LOGI("PuppyC spoofed statfs for path: %s", path);
    }
    return ret;
}
static int hook_statfs64(const char *path, struct statfs64 *buf) {
    int ret = orig_statfs64(path, buf);
    if (ret == 0 && buf != NULL) {
        long long jittered_free = apply_jitter(SPECS_FREE_SPACE, 0.0005);
        buf->f_blocks = SPECS_TOTAL_SPACE / buf->f_bsize;
        buf->f_bfree = jittered_free / buf->f_bsize;
        buf->f_bavail = buf->f_bfree; 
        LOGI("PuppyC spoofed statfs64 for path: %s", path);
    }
    return ret;
}
static int hook_statvfs(const char *path, struct statvfs *buf) {
    int ret = orig_statvfs(path, buf);
    if (ret == 0 && buf != NULL) {
        long long jittered_free = apply_jitter(SPECS_FREE_SPACE, 0.0005);
        buf->f_blocks = SPECS_TOTAL_SPACE / buf->f_frsize;
        buf->f_bfree = jittered_free / buf->f_frsize;
        buf->f_bavail = buf->f_bfree;
        LOGI("PuppyC spoofed statvfs for path: %s", path);
    }
    return ret;
}
static int hook_statvfs64(const char *path, struct statvfs64 *buf) {
    int ret = orig_statvfs64(path, buf);
    if (ret == 0 && buf != NULL) {
        long long jittered_free = apply_jitter(SPECS_FREE_SPACE, 0.0005);
        buf->f_blocks = SPECS_TOTAL_SPACE / buf->f_frsize;
        buf->f_bfree = jittered_free / buf->f_frsize;
        buf->f_bavail = buf->f_bfree;
        LOGI("PuppyC spoofed statvfs64 for path: %s", path);
    }
    return ret;
}
#endif

#if defined(DEVICE_CPU_INFO) || (defined(SPECS_TOTAL_RAM) && defined(SPECS_AVAIL_RAM) && defined(SPECS_SWAP_TOTAL_KB))
static int get_intercepted_fd(const char *path);

static int hook_openat(int dirfd, const char *path, int flags, int mode) {
    if (is_hidden_path(path)) {
        RLOG("hide openat %s", path);
        errno = ENOENT;
        return -1;
    }
    int fake_fd = get_intercepted_fd(path);
    if (fake_fd >= 0) {
        return fake_fd;
    }
    return orig_openat(dirfd, path, flags, mode);
}

// bionic's open() goes straight to the openat syscall rather than the libc
// openat() function we hook above, so a Java FileInputStream/FileReader (which
// uses open()) would bypass the /proc/* interception. Hook open() as well.
static int hook_open(const char *path, int flags, int mode) {
    if (is_hidden_path(path)) {
        RLOG("hide open %s", path);
        errno = ENOENT;
        return -1;
    }
    int fake_fd = get_intercepted_fd(path);
    if (fake_fd >= 0) {
        return fake_fd;
    }
    return orig_open(path, flags, mode);
}

// File-existence probes used by root detection: access()/faccessat() (what
// java.io.File.exists() calls) and stat()/lstat() (File.isFile/isDirectory).
// Report ENOENT for hidden paths; the stat buffer is never touched, so its
// layout is irrelevant (declared void* to avoid depending on struct stat).
typedef int (*AccessFunc)(const char *path, int mode);
typedef int (*FaccessatFunc)(int dirfd, const char *path, int mode, int flags);
typedef int (*StatFunc)(const char *path, void *buf);
typedef int (*FstatatFunc)(int dirfd, const char *path, void *buf, int flags);
static AccessFunc orig_access = NULL;
static FaccessatFunc orig_faccessat = NULL;
static StatFunc orig_stat = NULL;
static StatFunc orig_lstat = NULL;
static FstatatFunc orig_fstatat = NULL;

static int hook_access(const char *path, int mode) {
    if (is_hidden_path(path)) { RLOG("hide access %s", path); errno = ENOENT; return -1; }
    return orig_access(path, mode);
}
static int hook_faccessat(int dirfd, const char *path, int mode, int flags) {
    if (is_hidden_path(path)) { RLOG("hide faccessat %s", path); errno = ENOENT; return -1; }
    return orig_faccessat(dirfd, path, mode, flags);
}
static int hook_stat(const char *path, void *buf) {
    if (is_hidden_path(path)) { RLOG("hide stat %s", path); errno = ENOENT; return -1; }
    return orig_stat(path, buf);
}
static int hook_lstat(const char *path, void *buf) {
    if (is_hidden_path(path)) { RLOG("hide lstat %s", path); errno = ENOENT; return -1; }
    return orig_lstat(path, buf);
}
// On many bionic builds stat()/lstat() are inline wrappers around fstatat, so
// hook it too (this is also what java.io.File's stat path ultimately calls).
static int hook_fstatat(int dirfd, const char *path, void *buf, int flags) {
    if (is_hidden_path(path)) { RLOG("hide fstatat %s", path); errno = ENOENT; return -1; }
    return orig_fstatat(dirfd, path, buf, flags);
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
    if (written != (ssize_t)size) {
        close(fd);
        return -1;
    }
    lseek(fd, 0, SEEK_SET);
    return fd;
}

// Serve a copy of a mounts file (/proc/mounts, /proc/self/mounts|mountinfo) with
// Magisk/root mount lines stripped, so root detection that scans the mount table
// finds nothing. The real file is opened via the openat syscall directly to
// bypass our own open/openat hooks (no recursion). Returns -1 on any failure so
// the caller falls through to the real file.
// Serve a copy of a line-oriented /proc file with any line containing one of the
// `drop` substrings removed, via a memfd. The real file is opened through the
// openat syscall directly (bypassing our own hooks → no recursion). `cap` bounds
// the buffer (maps can be large). Returns -1 on failure so the caller falls
// through to the real file.
static int create_filtered_fd(const char *path, const char *const *drop,
                              const char *memfd_name, size_t cap) {
    int rfd = (int) syscall(__NR_openat, AT_FDCWD, path, O_RDONLY | O_CLOEXEC);
    if (rfd < 0) return -1;

    char *raw = (char *) malloc(cap);
    char *out = (char *) malloc(cap);
    if (raw == NULL || out == NULL) {
        free(raw); free(out); close(rfd);
        return -1;
    }

    size_t total = 0;
    ssize_t n;
    while (total < cap - 1 && (n = read(rfd, raw + total, cap - 1 - total)) > 0) {
        total += (size_t) n;
    }
    close(rfd);
    raw[total] = '\0';

    size_t olen = 0;
    char *saveptr = NULL;
    for (char *line = strtok_r(raw, "\n", &saveptr); line != NULL; line = strtok_r(NULL, "\n", &saveptr)) {
        int drop_line = 0;
        for (int i = 0; drop[i] != NULL; i++) {
            if (strstr(line, drop[i]) != NULL) { drop_line = 1; break; }
        }
        if (drop_line) continue;
        size_t len = strlen(line);
        if (olen + len + 1 < cap) {
            memcpy(out + olen, line, len);
            olen += len;
            out[olen++] = '\n';
        }
    }

    int fd = create_memfd_with_data(memfd_name, out, olen);
    free(raw);
    free(out);
    return fd;
}

// ---------------------------------------------------------------------------
// Anti-Frida fingerprinting. The embedded frida-gum/GumJS runtime spawns worker
// threads with tell-tale names (`gum-js-loop`, GLib's `gmain`/`gdbus`, …) that a
// Frida-aware app finds by reading /proc/<pid>/task/*/comm. We rename them to
// innocuous names at the source (hooking the thread-naming calls) and sweep any
// already-created ones, and we zero out TracerPid and hide libcoyote from the
// per-task maps. See TODO.md (Tier 1).
// ---------------------------------------------------------------------------
static int name_is_instrumentation(const char *n) {
    if (n == NULL) return 0;
    static const char *const tells[] = {"gum", "gmain", "gdbus", "frida",
                                         "linjector", "js-loop", "pool-frida", NULL};
    for (int i = 0; tells[i] != NULL; i++) {
        if (strstr(n, tells[i]) != NULL) return 1;
    }
    return 0;
}

// Plausible, common Android/JVM worker-thread names used to replace the ones
// above; distinct so renamed threads don't all collide on one suspicious name.
static const char *benign_thread_name(void) {
    static const char *const names[] = {
        "pool-1-thread-1", "pool-2-thread-1", "queued-work-looper",
        "Profile Saver", "Jit thread pool", "binder:worker",
    };
    static volatile int idx = 0;
    int i = __sync_fetch_and_add(&idx, 1);
    return names[i % (int)(sizeof(names) / sizeof(names[0]))];
}

typedef int (*SetNameFunc)(pthread_t, const char *);
typedef int (*PrctlFunc)(int, unsigned long, unsigned long, unsigned long, unsigned long);
static SetNameFunc orig_pthread_setname_np = NULL;
static PrctlFunc orig_prctl = NULL;

static int hook_pthread_setname_np(pthread_t t, const char *name) {
    if (name_is_instrumentation(name)) {
        return orig_pthread_setname_np(t, benign_thread_name());
    }
    return orig_pthread_setname_np(t, name);
}

static int hook_prctl(int option, unsigned long a2, unsigned long a3,
                      unsigned long a4, unsigned long a5) {
    if (option == PR_SET_NAME && name_is_instrumentation((const char *)a2)) {
        return orig_prctl(option, (unsigned long)benign_thread_name(), a3, a4, a5);
    }
    return orig_prctl(option, a2, a3, a4, a5);
}

// Rename already-running instrumentation threads (those created during
// gum_init_embedded, before the hooks above were installed) by writing to their
// comm file. Called once from apply_native_gum_hooks.
static void rename_instrumentation_threads(void) {
    DIR *d = opendir("/proc/self/task");
    if (d == NULL) return;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (e->d_name[0] < '0' || e->d_name[0] > '9') continue;
        char p[128];
        snprintf(p, sizeof(p), "/proc/self/task/%s/comm", e->d_name);
        int rfd = orig_open ? orig_open(p, O_RDONLY, 0) : open(p, O_RDONLY);
        if (rfd < 0) continue;
        char nm[64] = {0};
        ssize_t n = read(rfd, nm, sizeof(nm) - 1);
        close(rfd);
        if (n <= 0) continue;
        if (nm[n - 1] == '\n') nm[n - 1] = '\0';
        if (!name_is_instrumentation(nm)) continue;
        int wfd = orig_open ? orig_open(p, O_WRONLY, 0) : open(p, O_WRONLY);
        if (wfd < 0) continue;
        const char *b = benign_thread_name();
        (void)!write(wfd, b, strlen(b));
        close(wfd);
    }
    closedir(d);
}

// Serve a copy of /proc/<pid>/status with TracerPid forced to 0 (a non-zero
// TracerPid is a classic "I'm being traced" tell).
static int create_status_fd(const char *path) {
    int rfd = orig_open ? orig_open(path, O_RDONLY, 0) : open(path, O_RDONLY);
    if (rfd < 0) return -1;
    size_t cap = 65536;
    char *raw = (char *)malloc(cap);
    char *out = (char *)malloc(cap);
    if (raw == NULL || out == NULL) { free(raw); free(out); close(rfd); return -1; }
    size_t total = 0;
    ssize_t n;
    while (total < cap - 1 && (n = read(rfd, raw + total, cap - 1 - total)) > 0) {
        total += (size_t)n;
    }
    close(rfd);
    raw[total] = '\0';

    size_t olen = 0;
    char *saveptr = NULL;
    for (char *line = strtok_r(raw, "\n", &saveptr); line != NULL; line = strtok_r(NULL, "\n", &saveptr)) {
        const char *emit = line;
        if (strncmp(line, "TracerPid:", 10) == 0) emit = "TracerPid:\t0";
        size_t len = strlen(emit);
        if (olen + len + 1 < cap) {
            memcpy(out + olen, emit, len);
            olen += len;
            out[olen++] = '\n';
        }
    }
    int fd = create_memfd_with_data("status", out, olen);
    free(raw);
    free(out);
    return fd;
}

// True for /proc/.../comm, /proc/.../status, /proc/.../maps (self or any pid,
// incl. per-task), so the checks below catch the dynamic task paths too.
static int proc_basename_is(const char *path, const char *base) {
    if (strncmp(path, "/proc/", 6) != 0) return 0;
    const char *slash = strrchr(path, '/');
    return slash != NULL && strcmp(slash + 1, base) == 0;
}

static int get_intercepted_fd(const char *path) {
    if (path == NULL) return -1;

    // Hide gum/GLib worker-thread names from a /proc/.../comm read (a renamed
    // thread normally covers this, but back-stop any the rename missed).
    if (proc_basename_is(path, "comm")) {
        int rfd = orig_open ? orig_open(path, O_RDONLY, 0) : open(path, O_RDONLY);
        if (rfd >= 0) {
            char nm[64] = {0};
            ssize_t n = read(rfd, nm, sizeof(nm) - 1);
            close(rfd);
            if (n > 0) {
                char tmp[64];
                memcpy(tmp, nm, (size_t)n);
                tmp[n] = '\0';
                if (tmp[n - 1] == '\n') tmp[n - 1] = '\0';
                if (name_is_instrumentation(tmp)) {
                    char buf[64];
                    int len = snprintf(buf, sizeof(buf), "%s\n", benign_thread_name());
                    LOGI("PuppyC scrubbed instrumentation thread name from %s", path);
                    return create_memfd_with_data("comm", buf, (size_t)len);
                }
            }
        }
        return -1; // real thread name is fine
    }

    // TracerPid -> 0 for any /proc/<pid>/status (or per-task status).
    if (proc_basename_is(path, "status")) {
        int fd = create_status_fd(path);
        if (fd >= 0) {
            RLOG("status TracerPid scrubbed %s", path);
            return fd;
        }
    }

    if (strcmp(path, "/proc/mounts") == 0 ||
        strcmp(path, "/proc/self/mounts") == 0 ||
        strcmp(path, "/proc/self/mountinfo") == 0) {
        static const char *const drop[] = {"magisk", "/data/adb", "kernelsu", "KSU", NULL};
        int fd = create_filtered_fd(path, drop, "mounts", 65536);
        if (fd >= 0) {
            LOGI("PuppyC filtered Magisk mounts from %s", path);
            RLOG("filter mounts %s", path);
            return fd;
        }
    }

    // Hide the injected library from a /proc/self/maps scan (anti-tamper checks
    // enumerate mapped libraries looking for an injected .so). The embedded
    // GumJS runtime is linked into libcoyote.so, so dropping its lines hides it.
    if (proc_basename_is(path, "maps")) {
        static const char *const drop[] = {COYOTE_SO_NAME, NULL};
        int fd = create_filtered_fd(path, drop, "maps", 4 * 1024 * 1024);
        if (fd >= 0) {
            LOGI("PuppyC filtered agent lib from %s", path);
            RLOG("filter maps %s", path);
            return fd;
        }
    }

    // Spoof /proc/uptime: emulators/fresh sandboxes report a tiny uptime, which
    // is a common tell. Base it on the real boot clock + a constant offset so
    // the device looks booted days ago and the value still advances naturally
    // across reads. Done here (not by hooking the managed SystemClock) because
    // those clocks are far too hot to wrap.
    if (strcmp(path, "/proc/uptime") == 0) {
        RLOG("open /proc/uptime [spoofed]");
        struct timespec ts;
        double up = 300000.0; // ~3.5 day fallback if the clock read fails
        if (clock_gettime(CLOCK_BOOTTIME, &ts) == 0) {
            up = (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
        }
        double spoof_up = up + (4.0 * 24 * 60 * 60); // +4 days
        double spoof_idle = spoof_up * 3.7;          // plausible summed-core idle
        char buf[64];
        int len = snprintf(buf, sizeof(buf), "%.2f %.2f\n", spoof_up, spoof_idle);
        if (len > 0) {
            LOGI("PuppyC intercepted open for /proc/uptime");
            return create_memfd_with_data("uptime", buf, (size_t)len);
        }
    }

    #ifdef DEVICE_CPU_INFO
    if (strcmp(path, "/proc/cpuinfo") == 0) {
        LOGI("PuppyC intercepted open for /proc/cpuinfo");
        return create_memfd_with_data("cpuinfo", DEVICE_CPU_INFO, strlen(DEVICE_CPU_INFO));
    }
    #endif
    
    if (strcmp(path, "/sys/fs/selinux/enforce") == 0) {
        LOGI("PuppyC intercepted open for /sys/fs/selinux/enforce");
        const char *val = "1\n";
        return create_memfd_with_data("enforce", val, strlen(val));
    }
    
    #if defined(SPECS_TOTAL_RAM) && defined(SPECS_AVAIL_RAM) && defined(SPECS_SWAP_TOTAL_KB)
    if (strcmp(path, "/proc/meminfo") == 0) {
        LOGI("PuppyC intercepted open for /proc/meminfo");
        
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
        
        return create_memfd_with_data("meminfo", meminfo_buf, len);
    }
    #endif

    return -1;
}
#endif

#if defined(DEVICE_GL_VENDOR) || defined(DEVICE_GL_RENDERER) || defined(DEVICE_GL_VERSION)
static const char *hook_glGetString(unsigned int name) {
    #ifdef DEVICE_GL_VENDOR
    if (name == GL_VENDOR) {
        LOGI("PuppyC spoofed GL_VENDOR");
        return DEVICE_GL_VENDOR;
    }
    #endif

    #ifdef DEVICE_GL_RENDERER
    if (name == GL_RENDERER) {
        LOGI("PuppyC spoofed GL_RENDERER");
        return DEVICE_GL_RENDERER;
    }
    #endif

    #ifdef DEVICE_GL_VERSION
    if (name == GL_VERSION) {
        LOGI("PuppyC spoofed GL_VERSION");
        return DEVICE_GL_VERSION;
    }
    #endif

    if (orig_glGetString != NULL) {
        return orig_glGetString(name);
    }
    return "";
}
#endif

#ifdef DEVICE_GL_RENDERER
static void hook_vkGetPhysicalDeviceProperties(void* physicalDevice, void* pProperties) {
    if (real_vkGetPhysicalDeviceProperties) {
        real_vkGetPhysicalDeviceProperties(physicalDevice, pProperties);
        
        if (pProperties) {
            uint32_t *vendorID = (uint32_t *)((char *)pProperties + 8);
            *vendorID = 0x5143; 
            
            char *deviceName = (char *)pProperties + 20;
            strncpy(deviceName, DEVICE_GL_RENDERER, 255);
            deviceName[255] = '\0';
        }
    }
}

static PFN_vkVoidFunction hook_vkGetInstanceProcAddr(void* instance, const char* pName) {
    PFN_vkVoidFunction real_func = orig_vkGetInstanceProcAddr(instance, pName);
    
    if (pName != NULL && strcmp(pName, "vkGetPhysicalDeviceProperties") == 0) {
        if (real_func != NULL) {
            real_vkGetPhysicalDeviceProperties = (VkGetPhysicalDevicePropertiesFunc)real_func;
            return (PFN_vkVoidFunction)hook_vkGetPhysicalDeviceProperties;
        }
    }
    
    return real_func;
}
#endif

// Anti-debug neutralization. Apps commonly call ptrace(PTRACE_TRACEME) to detect
// a debugger/instrumentation (it fails if the process is already being traced)
// or to self-attach as a defense. Report success for TRACEME without actually
// tracing, and pass everything else through.
#define PT_TRACE_ME 0 // PTRACE_TRACEME
static long hook_ptrace(int request, pid_t pid, void *addr, void *data) {
    RLOG("ptrace request=%d", request);
    if (request == PT_TRACE_ME) {
        return 0;
    }
    return orig_ptrace(request, pid, addr, data);
}

void apply_native_gum_hooks() {
    LOGI("PuppyC waking up in the background... sniffing out environment.");
    GumInterceptor *interceptor = gum_interceptor_obtain();
    if (interceptor == NULL) {
        LOGE("PuppyC lost the scent: failed to obtain GumInterceptor.");
    }

    gum_interceptor_begin_transaction(interceptor);

    // 1. System Property Hooks
    #ifdef SYSTEM_PROP_OVERRIDES_COUNT
    gpointer target_prop_get        = GSIZE_TO_POINTER(dlsym(RTLD_DEFAULT, "__system_property_get"));
    gpointer target_prop_read_cb    = GSIZE_TO_POINTER(dlsym(RTLD_DEFAULT, "__system_property_read_callback"));
    gpointer target_prop_find       = GSIZE_TO_POINTER(dlsym(RTLD_DEFAULT, "__system_property_find"));
    if (target_prop_get)        gum_interceptor_replace(interceptor, target_prop_get, GSIZE_TO_POINTER(hook_system_property_get), (gpointer *)&orig_system_property_get, NULL);
    if (target_prop_read_cb)    gum_interceptor_replace(interceptor, target_prop_read_cb, GSIZE_TO_POINTER(hook_system_property_read_callback), (gpointer *)&orig_system_property_read_callback, NULL);
    if (target_prop_find)       gum_interceptor_replace(interceptor, target_prop_find, GSIZE_TO_POINTER(hook_system_property_find), (gpointer *)&orig_system_property_find, NULL);

    gpointer t_posix_spawn  = GSIZE_TO_POINTER(dlsym(RTLD_DEFAULT, "posix_spawn"));
    gpointer t_posix_spawnp = GSIZE_TO_POINTER(dlsym(RTLD_DEFAULT, "posix_spawnp"));
    gpointer t_execve       = GSIZE_TO_POINTER(dlsym(RTLD_DEFAULT, "execve"));
    gpointer t_execv        = GSIZE_TO_POINTER(dlsym(RTLD_DEFAULT, "execv"));
    gpointer t_execvp       = GSIZE_TO_POINTER(dlsym(RTLD_DEFAULT, "execvp"));
    gpointer t_execvpe      = GSIZE_TO_POINTER(dlsym(RTLD_DEFAULT, "execvpe"));
    if (t_posix_spawn)  gum_interceptor_replace(interceptor, t_posix_spawn,  GSIZE_TO_POINTER(hook_posix_spawn),  (gpointer *)&orig_posix_spawn,  NULL);
    if (t_posix_spawnp) gum_interceptor_replace(interceptor, t_posix_spawnp, GSIZE_TO_POINTER(hook_posix_spawnp), (gpointer *)&orig_posix_spawnp, NULL);
    if (t_execve)       gum_interceptor_replace(interceptor, t_execve,       GSIZE_TO_POINTER(hook_execve),       (gpointer *)&orig_execve,       NULL);
    if (t_execv)        gum_interceptor_replace(interceptor, t_execv,        GSIZE_TO_POINTER(hook_execv),        NULL,        NULL);
    if (t_execvp)       gum_interceptor_replace(interceptor, t_execvp,       GSIZE_TO_POINTER(hook_execvp),       NULL,       NULL);
    if (t_execvpe)      gum_interceptor_replace(interceptor, t_execvpe,      GSIZE_TO_POINTER(hook_execvpe),      (gpointer *)&orig_execvpe,      NULL);

    // Anti-debug: neutralize ptrace(PTRACE_TRACEME)-based detection.
    gpointer t_ptrace = GSIZE_TO_POINTER(dlsym(RTLD_DEFAULT, "ptrace"));
    if (t_ptrace) gum_interceptor_replace(interceptor, t_ptrace, GSIZE_TO_POINTER(hook_ptrace), (gpointer *)&orig_ptrace, NULL);

    // Anti-Frida: rename the embedded runtime's worker threads (gum-js-loop,
    // gmain, …) to innocuous names so /proc/.../comm scans don't reveal them.
    gpointer t_setname = GSIZE_TO_POINTER(dlsym(RTLD_DEFAULT, "pthread_setname_np"));
    if (t_setname) gum_interceptor_replace(interceptor, t_setname, GSIZE_TO_POINTER(hook_pthread_setname_np), (gpointer *)&orig_pthread_setname_np, NULL);
    gpointer t_prctl = GSIZE_TO_POINTER(dlsym(RTLD_DEFAULT, "prctl"));
    if (t_prctl) gum_interceptor_replace(interceptor, t_prctl, GSIZE_TO_POINTER(hook_prctl), (gpointer *)&orig_prctl, NULL);
    #endif

    #if defined(SPECS_WIDTH) || defined(SPECS_HEIGHT)
    void *libnativewindow = dlopen("libnativewindow.so", RTLD_LAZY);

    #ifdef SPECS_WIDTH
    gpointer target_width = libnativewindow ? dlsym(libnativewindow, "ANativeWindow_getWidth") : NULL;
    if (target_width) gum_interceptor_replace(interceptor, target_width, GSIZE_TO_POINTER(hook_anativewindow_get_width), (gpointer *)&orig_anativewindow_get_width, NULL);
    #endif

    #ifdef SPECS_HEIGHT
    gpointer target_height = libnativewindow ? dlsym(libnativewindow, "ANativeWindow_getHeight") : NULL;
    if (target_height) gum_interceptor_replace(interceptor, target_height, GSIZE_TO_POINTER(hook_anativewindow_get_height), (gpointer *)&orig_anativewindow_get_height, NULL);
    #endif
    #endif

    #if defined(SPECS_FREE_SPACE) && defined(SPECS_TOTAL_SPACE)
    gpointer target_statfs      = GSIZE_TO_POINTER(dlsym(RTLD_DEFAULT, "statfs"));
    gpointer target_statfs64    = GSIZE_TO_POINTER(dlsym(RTLD_DEFAULT, "statfs64"));
    gpointer target_statvfs     = GSIZE_TO_POINTER(dlsym(RTLD_DEFAULT, "statvfs"));
    gpointer target_statvfs64   = GSIZE_TO_POINTER(dlsym(RTLD_DEFAULT, "statvfs64"));
    if (target_statfs)      gum_interceptor_replace(interceptor, target_statfs, GSIZE_TO_POINTER(hook_statfs), (gpointer *)&orig_statfs, NULL);
    if (target_statfs64)    gum_interceptor_replace(interceptor, target_statfs64, GSIZE_TO_POINTER(hook_statfs64), (gpointer *)&orig_statfs64, NULL);
    if (target_statvfs)     gum_interceptor_replace(interceptor, target_statvfs, GSIZE_TO_POINTER(hook_statvfs), (gpointer *)&orig_statvfs, NULL);
    if (target_statvfs64)   gum_interceptor_replace(interceptor, target_statvfs64, GSIZE_TO_POINTER(hook_statvfs64), (gpointer *)&orig_statvfs64, NULL);
    #endif

    #if defined(DEVICE_CPU_INFO) || (defined(SPECS_TOTAL_RAM) && defined(SPECS_AVAIL_RAM) && defined(SPECS_SWAP_TOTAL_KB))
    gpointer target_openat = GSIZE_TO_POINTER(dlsym(RTLD_DEFAULT, "openat"));
    if (target_openat) gum_interceptor_replace(interceptor, target_openat, GSIZE_TO_POINTER(hook_openat), (gpointer *)&orig_openat, NULL);

    gpointer target_open = GSIZE_TO_POINTER(dlsym(RTLD_DEFAULT, "open"));
    if (target_open) gum_interceptor_replace(interceptor, target_open, GSIZE_TO_POINTER(hook_open), (gpointer *)&orig_open, NULL);

    // Root/Magisk file-existence hiding.
    gpointer target_access    = GSIZE_TO_POINTER(dlsym(RTLD_DEFAULT, "access"));
    gpointer target_faccessat = GSIZE_TO_POINTER(dlsym(RTLD_DEFAULT, "faccessat"));
    gpointer target_stat      = GSIZE_TO_POINTER(dlsym(RTLD_DEFAULT, "stat"));
    gpointer target_lstat     = GSIZE_TO_POINTER(dlsym(RTLD_DEFAULT, "lstat"));
    if (target_access)    gum_interceptor_replace(interceptor, target_access,    GSIZE_TO_POINTER(hook_access),    (gpointer *)&orig_access,    NULL);
    if (target_faccessat) gum_interceptor_replace(interceptor, target_faccessat, GSIZE_TO_POINTER(hook_faccessat), (gpointer *)&orig_faccessat, NULL);
    if (target_stat)      gum_interceptor_replace(interceptor, target_stat,      GSIZE_TO_POINTER(hook_stat),      (gpointer *)&orig_stat,      NULL);
    if (target_lstat)     gum_interceptor_replace(interceptor, target_lstat,     GSIZE_TO_POINTER(hook_lstat),     (gpointer *)&orig_lstat,     NULL);
    // fstatat / newfstatat (stat/lstat are often inline wrappers around it).
    gpointer target_fstatat = GSIZE_TO_POINTER(dlsym(RTLD_DEFAULT, "fstatat"));
    if (!target_fstatat) target_fstatat = GSIZE_TO_POINTER(dlsym(RTLD_DEFAULT, "newfstatat"));
    if (target_fstatat) gum_interceptor_replace(interceptor, target_fstatat, GSIZE_TO_POINTER(hook_fstatat), (gpointer *)&orig_fstatat, NULL);
    #endif

    #if defined(DEVICE_GL_VENDOR) || defined(DEVICE_GL_RENDERER) || defined(DEVICE_GL_VERSION)
    void *libgles = dlopen("libGLESv2.so", RTLD_LAZY);
    gpointer target_glGetString = libgles ? dlsym(libgles, "glGetString") : NULL;
    
    if (!target_glGetString) target_glGetString = GSIZE_TO_POINTER(dlsym(RTLD_DEFAULT, "glGetString"));
    if (target_glGetString) gum_interceptor_replace(interceptor, target_glGetString, GSIZE_TO_POINTER(hook_glGetString), (gpointer *)&orig_glGetString, NULL);
    #endif

    #ifdef DEVICE_GL_RENDERER
    void *libvulkan = dlopen("libvulkan.so", RTLD_LAZY);
    if (libvulkan) {
        gpointer target_vk = dlsym(libvulkan, "vkGetInstanceProcAddr");
        if (target_vk) {
            gum_interceptor_replace(interceptor, target_vk, GSIZE_TO_POINTER(hook_vkGetInstanceProcAddr), (gpointer *)&orig_vkGetInstanceProcAddr, NULL);
            LOGI("Applied Vulkan hooks!");
        } else {
            LOGI("vkGetInstanceProcAddr not found in libvulkan.so.");
        }
    }
    #endif

    gum_interceptor_end_transaction(interceptor);

    // Rename any instrumentation threads that were already running before the
    // thread-naming hooks above were installed (e.g. GLib threads spun up inside
    // gum_init_embedded). Future ones are caught by the hooks.
    rename_instrumentation_threads();

    LOGI("Native Gum hooks applied.");
}