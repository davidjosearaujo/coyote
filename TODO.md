# Coyote — Improvements TODO

Prioritized improvements identified from real-device testing (physical arm64
Pixel 7 + emulator). Checked items are done.

## Tier 1 — Stealth: hide Frida/Coyote's own fingerprints

Coyote defeats emulator/device detection well, but the frida-gum runtime leaves
classic tells that a Frida-aware app would catch. This is the most on-mission
gap.

- [x] Rename the gum + GLib worker threads (`gum-js-loop`, `gmain`, `gdbus`, …)
      to innocuous names via hooks on `prctl(PR_SET_NAME)` + `pthread_setname_np`,
      plus a startup sweep of `/proc/self/task/*/comm` for ones named before the
      hooks were installed.
- [x] Intercept reads of `/proc/.../comm` (back-stop) — scrub any instrumentation
      name that slips past the rename.
- [x] Spoof `/proc/<pid>/status` `TracerPid:` to `0` (anti-debug / anti-trace).
- [x] Hook the per-task maps path `/proc/.../maps` (was only `/proc/self/maps`
      and `/proc/self/task/maps`).
- [x] Remove the fixed on-disk signature: the injected library name is now
      randomized per run (`-DCOYOTE_SO_NAME`, e.g. `libnixifsfj.so`) instead of
      `libcoyote.so`, and the native maps filter / log key off that name.
      - Note: hiding the `.so` from `stat()`/`access()` and from `readdir()` was
        tried and reverted — hiding a *mapped* library from `stat` makes the
        linker/ART get `ENOENT` and breaks Java-bridge hook installation (gate
        never fires), and `readdir` is far too hot. Name randomization +
        in-process `/proc/.../maps` filtering cover the real vectors safely.
- [x] Lower priority: scrub `frida`/`gum`/`gadget` strings in mapped memory and
      `/proc/self/fd` entries. The `/proc/.../maps` filter now also drops any
      `frida`/`gadget`/`linjector`/`gum-js` mapping (a maps scan is how those
      strings surface in "mapped memory"), and `readlink`/`readlinkat` are hooked
      to rewrite `/proc/<pid>/fd/*` symlinks that point at the agent lib or a
      frida/gum-named memfd to `/dev/null`. Deliberately *not* done: rewriting the
      literal bytes in live `/proc/self/mem` / heap — it would corrupt the running
      GumJS runtime, and (per the original note) frida-*server* socket tells don't
      apply to Coyote's embedded GumJS. Not yet verified on a physical device.
      (Named-pipe / `linjector` tells are frida-*server* specific; Coyote embeds
      GumJS, so there's no control socket to hide.)
- [x] Verify on the Pixel 7: `/proc/<pid>/task/*/comm` shows no frida-shaped
      names (all look like normal Android threads); no correctness regression
      (Border Patrol unchanged vs without Tier 1).

## Bugs found during testing

- [x] **WifiInfo spoof is flaky on a physical device.** Root cause was NOT in
      the `wifi` block (its `getSSID`/`getBSSID`/`getMacAddress` hooks are
      correct). The real bug was in `installGate`: it armed a hook on
      `Instrumentation.callActivityOnCreate` and installed the whole Java hook
      set only when that hook *fired*. Arming is the first use of
      frida-java-bridge (~300ms of class-model + deopt warmup), and on the fast
      physical Pixel 7 the app's main thread entered its first `onCreate`
      *before* arming finished, so the gate hook never saw the call and
      **NO Java hook ever installed** — every Java-level vector (wifi, display,
      RAM, telephony, …) leaked the real value. It looked "wifi-specific" only
      because wifi was the vector being watched. Reproduced 3/3 on the Pixel 7
      (gate armed, never fired, 283/314).
      - Fix: install the hooks eagerly the moment `Java.available` is true
        (`runOnce('eager')`), instead of waiting for a lifecycle gate. This is
        the earliest possible point and runs with fewer live app threads than at
        `onCreate`, so the historical all-thread-suspend deadlock risk the gate
        was avoiding is no worse (verified on the heavy cocos game Top War —
        `Displayed +1.16s`, no ANR/deadlock). The only fallback, kept for a ROM
        whose early ART cannot yet deopt from our thread (the eager
        `Java.performNow` faults), is the pre-existing `waitForApplication`: it
        polls until the Application is bound and installs then (pre-onCreate,
        post-bootstrap). The old `callActivityOnCreate` gate is gone — eager
        install supersedes it.
      - Verified on the Pixel 7: WifiInfo (`getSSID`/`getBSSID`/`getMacAddress`)
        now resolves to the profile values every run; no wifi FAIL rows. Pass
        count 283 → ~292-294 (see the early-read limitation below for the
        residual ~20).

- [x] **Build break: profiles without a `system_prop_overrides` section failed
      to compile.** `is_hidden_path()` in `puppyc.c` lived inside the
      `#ifdef SYSTEM_PROP_OVERRIDES_COUNT` block (lines 185-500) but is used by
      the file-op hooks in the independent SPECS/CPU block, so any profile
      lacking that section (including the shipped `pixel7_us_tmobile.json`) hit
      `call to undeclared function 'is_hidden_path'`. Border Patrol's test
      profile has the section, which masked it. Fix: moved `is_hidden_path` out
      of the guard so it is always compiled. Verified both profiles build.

## Tier 2 — Robustness & regression safety

- [x] pytest regression test (`test/test_border_patrol.py`): builds the agent,
      installs Border Patrol, runs it under Coyote, dumps the UI, and asserts the
      "✓ All N system checks passed" summary. Skips cleanly when there is no
      device / NDK / frida-tools. Catches timing regressions (e.g. the
      FLAG_SECURE-deferred one) automatically. Run with `pytest test/`.
      (Chose pytest over a `coyote selftest` subcommand.)
- [x] Quiet the `wrap` `SIGSYS`/`SECCOMP` crash: `coyote.c` installs a `SIGSYS`
      handler in the library constructor that emulates the blocked
      `setgid`/`setuid`/`setres*`/`setgroups` (writes 0 into the syscall-return
      register and resumes) during idmap/zygote-time init, so the transient
      resource-init process no longer dies every launch. Other trapped syscalls
      fall through to the previous disposition. (Backtrace: `setgid` →
      `ExecuteBinary` → `createIdmap` → `createImmutableFrameworkIdmapsInZygote`.)
      Verified on the Pixel 7: `[Coyote] SIGSYS credential-syscall shim installed`
      fires in every forked process and no `wrap`/SECCOMP crash occurs on launch.

## Tier 3 — UX / polish

- [x] When a `setprop` is rejected on a real device, print a one-line hint to use
      `--force-permissive` (shown once, with the count of rejected writes, and
      only when `--force-permissive` wasn't already passed).
- [x] Make a couple more best-effort ADB call sites non-fatal (`_exec(fatal=False)`),
      like `setprop` now is: the `am force-stop` / `monkey` launch in
      `launch_target_app` and the stop/clear steps in `reset_target_app`.
- [x] Doc note: the staging mechanism is now near-vestigial (only `ssl` /
      `user_agent` deferred) — documented in README as a safety valve for apps
      that deadlock installing a specific hook synchronously.

## Known limitations (documented, not necessarily actionable)

- Native-bridge (arm-on-x86) translation: `--nb-shim` injects the arm64 agent
  into the guest, but `gum_init_embedded()` crashes under ndk_translation, so
  translated apps can't be instrumented on an x86_64 emulator. Use a native-ABI
  target (arm64 image/device). Guarded with a clear message by default.

- Earliest-read Java vectors on a fast physical device: the families an app
  reads in the very first moments of `onCreate` can still leak the real value on
  fast hardware. On the Pixel 7 / Border Patrol the residual ~20 failures
  (293-294/314) are all of this kind: display metrics, total RAM
  (`ActivityManager.getMemoryInfo().totalMem`), SIM/telephony (operator, MCC+MNC,
  country ISO, SIM state), FLAG_SECURE stripping, battery level, the USB/ADB/
  `install_referrer` sticky-intent + `Settings.Global` reads, and active network
  type. Everything read even slightly later (wifi, read ~5s into `onCreate`, plus
  reflection/shell/sensors/root-hiding/location/build/cpu) is caught. Measured on
  the Pixel 7: the agent reaches `Java.available` at ~T, but frida-java-bridge's
  first-use warmup (~300ms) means the first hook can't land until ~T+300ms, and
  the app's main thread has already read these framework-cached values by then.
  Reordering the critical set to front-load these did not help: the reads happen
  essentially *at* `Java.available`, before any JS hook can install. Closing the
  gap needs either native-layer spoofing of these specific vectors (they flow
  through binder/ART caches, not `/proc`, so the existing native file hooks don't
  see them) or an early main-thread barrier that holds the app until the Java
  hooks are in — both non-trivial and the barrier carries real deadlock risk (see
  the `installGate` history). Not attempted. On a slower target (emulator), the
  agent wins this race and Border Patrol reaches 314, which is what the pytest
  asserts; on a fast physical device the pytest will report the partial count.
