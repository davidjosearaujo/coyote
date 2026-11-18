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

- [ ] **WifiInfo spoof is flaky on a physical device.** On the Pixel 7, Border
      Patrol's Wi-Fi vector (SSID/BSSID/MAC) intermittently fails even with
      `wifi` in the critical group — it gets the real anonymized values
      (`02:00:00:00:00:00`, `<unknown ssid>`) instead of the profile's. Not
      Tier-1-related (reproduces without it). Likely the `WifiInfo` method
      hooks (`getSSID`/`getBSSID`/`getMacAddress`) aren't resolving/installing
      reliably at gate time on Android 13, or the app reads a path the hook
      misses. Investigate the block's `Java.use`/overload resolution and whether
      `WifiManager.getConnectionInfo()` returns a pre-hook instance.

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
      Not yet verified on a physical device — needs a device + NDK run.

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
