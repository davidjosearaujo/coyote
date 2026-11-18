// _GNU_SOURCE exposes the x86/x86_64 REG_* register indices used by the SIGSYS
// shim (uc_mcontext.gregs[REG_RAX]/[REG_EAX]); harmless on arm/arm64.
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "frida-gumjs.h"

#include <stdio.h>          // LOGI macro (indirectly)
#include <stdlib.h>         // could be removed but harmless
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <dlfcn.h>
#include <limits.h>
#include <signal.h>
#include <ucontext.h>
#include <sys/syscall.h>
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

// --------------------------------------------------------------------------- //
// SIGSYS (seccomp) shim for credential-dropping syscalls.
//
// When Coyote is injected via `wrap.<pkg>` + LD_PRELOAD, the agent is also
// preloaded into the short-lived helper processes the app framework forks at
// launch (e.g. the idmap/resource-init process under `createIdmap` ->
// `createImmutableFrameworkIdmapsInZygote`). Those processes call
// setgid()/setuid() to drop privileges, but the app's seccomp filter traps the
// call with SIGSYS (SECCOMP_RET_TRAP) instead of allowing it, so the helper
// dies with a SIGSYS/SECCOMP crash on every launch — noisy, and it can abort
// the init step.
//
// We install a SIGSYS handler that, for exactly these credential-setting
// syscalls, emulates success: the kernel delivers SIGSYS *before* the syscall
// runs and with the PC past the trapping instruction, so writing 0 into the
// syscall-return register and returning makes the call look like it succeeded.
// Any other trapped syscall is passed to the previous disposition unchanged, so
// we never mask an unrelated seccomp violation.
// --------------------------------------------------------------------------- //
#ifndef SYS_SECCOMP
#define SYS_SECCOMP 1
#endif

static struct sigaction g_old_sigsys;

// Is `nr` one of the credential-setting syscalls we emulate on SIGSYS? Both the
// legacy 16-bit and the *32 variants are matched where the arch defines them
// (32-bit ARM uses the *32 forms).
static int coyote_is_emulated_cred_syscall(long nr) {
    switch (nr) {
#ifdef __NR_setuid
        case __NR_setuid:
#endif
#ifdef __NR_setuid32
        case __NR_setuid32:
#endif
#ifdef __NR_setgid
        case __NR_setgid:
#endif
#ifdef __NR_setgid32
        case __NR_setgid32:
#endif
#ifdef __NR_setreuid
        case __NR_setreuid:
#endif
#ifdef __NR_setreuid32
        case __NR_setreuid32:
#endif
#ifdef __NR_setregid
        case __NR_setregid:
#endif
#ifdef __NR_setregid32
        case __NR_setregid32:
#endif
#ifdef __NR_setresuid
        case __NR_setresuid:
#endif
#ifdef __NR_setresuid32
        case __NR_setresuid32:
#endif
#ifdef __NR_setresgid
        case __NR_setresgid:
#endif
#ifdef __NR_setresgid32
        case __NR_setresgid32:
#endif
#ifdef __NR_setfsuid
        case __NR_setfsuid:
#endif
#ifdef __NR_setfsuid32
        case __NR_setfsuid32:
#endif
#ifdef __NR_setfsgid
        case __NR_setfsgid:
#endif
#ifdef __NR_setfsgid32
        case __NR_setfsgid32:
#endif
#ifdef __NR_setgroups
        case __NR_setgroups:
#endif
#ifdef __NR_setgroups32
        case __NR_setgroups32:
#endif
            return 1;
        default:
            return 0;
    }
}

// Force the trapped syscall's return register to `rc` (0 = success).
static void coyote_set_syscall_retval(void *uctx, long rc) {
    ucontext_t *uc = (ucontext_t *)uctx;
    if (!uc) return;
#if defined(__aarch64__)
    uc->uc_mcontext.regs[0] = (unsigned long)rc;
#elif defined(__arm__)
    uc->uc_mcontext.arm_r0 = (unsigned long)rc;
#elif defined(__x86_64__)
    uc->uc_mcontext.gregs[REG_RAX] = (greg_t)rc;
#elif defined(__i386__)
    uc->uc_mcontext.gregs[REG_EAX] = (greg_t)rc;
#else
    (void)rc;
#endif
}

static void coyote_sigsys_handler(int sig, siginfo_t *info, void *uctx) {
    if (info && info->si_code == SYS_SECCOMP &&
        coyote_is_emulated_cred_syscall(info->si_syscall)) {
        // Emulate a successful privilege drop and resume past the trap.
        coyote_set_syscall_retval(uctx, 0);
        return;
    }

    // Not a syscall we emulate: defer to whatever was installed before us so we
    // don't swallow an unrelated seccomp violation.
    if (g_old_sigsys.sa_flags & SA_SIGINFO) {
        if (g_old_sigsys.sa_sigaction) {
            g_old_sigsys.sa_sigaction(sig, info, uctx);
            return;
        }
    } else if (g_old_sigsys.sa_handler == SIG_IGN) {
        return;
    } else if (g_old_sigsys.sa_handler && g_old_sigsys.sa_handler != SIG_DFL) {
        g_old_sigsys.sa_handler(sig);
        return;
    }
    // Default disposition: restore it and re-raise to preserve crash semantics.
    signal(SIGSYS, SIG_DFL);
    raise(SIGSYS);
}

// Install the SIGSYS shim as early as possible (synchronously in the library
// constructor, before any framework code in this process drops privileges).
static void coyote_install_sigsys_shim(void) {
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = coyote_sigsys_handler;
    sa.sa_flags = SA_SIGINFO | SA_RESTART | SA_NODEFER;
    sigemptyset(&sa.sa_mask);
    if (sigaction(SIGSYS, &sa, &g_old_sigsys) == 0) {
        LOGI("SIGSYS credential-syscall shim installed");
    }
}

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

    // Quiet the transient idmap/resource-init helper's SIGSYS/SECCOMP crash by
    // emulating the setgid/setuid it traps. Installed synchronously here (not on
    // the master thread) so it is in place before the framework drops privileges.
    coyote_install_sigsys_shim();

    pthread_t thread;
    if (pthread_create(&thread, NULL, coyote_master_thread, NULL) == 0) {
        pthread_detach(thread);
    }
}