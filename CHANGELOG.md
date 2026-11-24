# Changelog

All notable changes to Coyote are documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

## [2.3.0] - 2026-11-24

### Fixed
- **Java hooks could silently fail to install on a fast device (root cause of
  "flaky" per-vector spoofing).** The agent armed a gate on
  `Instrumentation.callActivityOnCreate` and installed the whole Java hook set
  only when that gate fired. Arming is the first use of frida-java-bridge
  (~300 ms of class-model + deopt warmup), and on a fast device/app (physical
  Pixel 7 / Android 13) the app entered its first `onCreate` before arming
  finished, so the gate never fired and **no** Java hook installed — every
  Java-level vector (Wi-Fi, display, RAM, telephony, …) leaked the real device
  value. Surfaced as intermittent Wi-Fi spoofing but affected every Java hook.
  The agent now installs eagerly the moment the VM is available
  (`Java.available`), the earliest possible point and with fewer live app
  threads than at `onCreate`, so the deadlock risk the gate was avoiding is no
  worse (verified deadlock-free on the heavy cocos game Top War). The lone
  fallback, for a ROM that cannot yet deopt from the agent's thread, is
  `waitForApplication` (install at Application-bound). The `callActivityOnCreate`
  gate, and the catch-up/watchdog that tried to patch its race, are removed.
- **Build failed for any profile without a `system_prop_overrides` section.**
  `is_hidden_path()` in `puppyc.c` was scoped inside the
  `#ifdef SYSTEM_PROP_OVERRIDES_COUNT` block but is called by the (independent)
  SPECS/CPU file-op hooks, so a profile lacking that section — including the
  shipped `pixel7_us_tmobile.json` — failed to compile with
  `undeclared function 'is_hidden_path'`. Moved it out of the guard so it is
  always compiled.
- **Physical-device support: Java/JS layer no longer aborts on fast devices.**
  `wrap`+`LD_PRELOAD` re-execs a fresh `app_process`, which re-creates ART from
  scratch; the agent raced it and the frida-java-bridge probed the VM before
  `Runtime::Start()` finished, aborting the whole process with
  `Check failed: runtime->IsStarted()`. The slow x86_64 emulator usually won the
  race; a fast arm64 phone (Pixel 7) lost it every time. The JS layer now gates
  on ART actually being started — it hooks `JNI_CreateJavaVM` (whose return marks
  `Runtime::Start()` completing) and waits for it, with a settle-and-proceed
  fallback when a VM is already present — before touching the bridge.
- **`configure_adb_properties` no longer aborts on a protected `setprop`.** Real
  devices reject many property writes that the permissive emulator allowed;
  these are now best-effort (run via `su`, warn-and-continue) instead of killing
  the whole run. `_exec` gained a `fatal=False` mode.
- Target-ABI detection is no longer skipped by `--skip-adb` (which is only meant
  to skip `setprop`); skipping it previously built with the wrong devkit arch
  and failed to link on a non-host-ABI device.

### Added
- **`--force-permissive`.** Sets SELinux permissive (`setenforce 0`) before
  applying the profile so the device accepts protected `setprop` writes that
  enforcing policy rejects. For dedicated analysis devices; optional (the
  native/JS spoof layers work without it).
- **ABI-aware builds.** Coyote now detects the *target app's* `primaryCpuAbi`
  (via `dumpsys package`) and compiles `libcoyote.so` for the ABI the app's
  process actually runs as, instead of always building for the device's primary
  ABI. The Frida devkit is fetched/cached per-arch (tracked by a
  `.devkit_arch` marker) and auto-switched when the build target changes. This
  makes 32-bit apps on 64-bit devices (and other native secondary-ABI cases)
  work correctly.
- **Native-bridge guard.** When the target app runs only through native-bridge
  translation (e.g. an arm64-only app on an x86_64 emulator), the plain
  `wrap`+`LD_PRELOAD` injection cannot cross into the translated process — the
  app would hang on its splash because its own native libraries fail to load in
  the forced non-native process. Coyote now detects this up front and stops
  with a clear message (build for a matching-ABI target, or use `--nb-shim`)
  instead of producing a silent splash hang.
- **`--nb-shim` (experimental).** Opt-in path to instrument a native-bridge
  app: installs a shim that re-execs the *guest* `app_process` and hands the
  agent to the guest linker via the translator's `NDK_TRANSLATION_GUEST_LD_PRELOAD`
  channel. The agent reliably loads into the translated guest, but frida-gum
  itself cannot initialize there: `gum_init_embedded()` segfaults (null deref)
  inside the ndk_translation guest, consistently, before any hooks apply — so
  both the native and GumJS layers are blocked under translation, not just the
  JS runtime. This is a frida-under-binary-translation limitation below Coyote's
  level. `--nb-shim` also requires SELinux permissive. Off by default.

### Changed
- Narrowed the hottest JS agent hooks so their per-call cost is minimal (they
  run on methods the framework calls thousands of times during startup, the
  likely cause of real-app splash hangs). All changes are behaviour-preserving
  — verified against Border Patrol (same pass set as unmodified `main`):
  - `Settings.$Global/$Secure/$System.getString/getInt`: precompute a per-class
    O(1) lookup map once instead of an `Array.indexOf` scan (+ `parseInt`) per
    call.
  - `BatteryManager` `Intent.getIntExtra` / `Bundle.getInt`: allocation-free —
    the old code built a fresh object literal on every call; pure-spoof keys now
    return a constant without invoking the original getter at all.
  - Install-referrer `Bundle.getString/getLong`: check the key before touching
    the original and hoist the timestamp map out of the per-call path.
  - `System.getProperty`: hook only the single-`String` overload (the one that
    carries `http.agent`) instead of every overload.
- JS agent hook install is now **staged**, with a `CRITICAL` / `DEFERRED` split
  in `agent.js`. Every hook family that answers a **device-identity** probe
  (Build, props, telephony, RAM, display, locale, sensors, package manager,
  FLAG_SECURE, battery, network/VPN, proxy, time, keyguard, USB, install
  referrer, accounts, Wi-Fi, location) installs synchronously **before the first
  activity's `onCreate`**, so an evasive app that reads them at startup never
  sees the real device. The full install is only tens of ms (the hang that
  motivated deferral turned out to be the native-bridge ABI mismatch, not the
  install cost). Only the **network-time** families (`ssl`, `user_agent`) — which
  take effect on the app's first TLS/HTTP traffic, not at startup — stay
  deferred and drain one-per-main-thread-tick via `Java.scheduleOnMainThread`
  once the UI is up. The staging mechanism remains so any block can be moved to
  `DEFERRED` if a specific app ever deadlocks installing it synchronously.
- Internal cleanup (behaviour-preserving): the three per-identifier random-string
  generators (`genHex`/`randomStr`/`generateRandomHash`) are consolidated into a
  single `randomString(len, alphabet)` helper, and `installGate` is simplified to
  the eager-install + `waitForApplication` fallback (the gate/catch-up/watchdog
  machinery is gone).

## [2.2.0] - 2026-10-23

### Added
- `pyproject.toml`: Coyote now installs as a Python package exposing two
  console commands, `coyote` and `coyote-profiles`.
- `assets/coyote-logo.svg` and a centered README header (logo, title, subtitle,
  badges).

### Changed
- Coyote is now an installable Python package. `pipx install .` (or
  `pip install .`) puts two commands on your `PATH`: `coyote` (the orchestrator,
  was `python coyote.py`) and `coyote-profiles` (the profile generator, was
  `python profiles/generate_profiles.py`). Source moved under `src/coyote/`;
  the C sources, JS agent and profile database ship as package data.
- Writable artifacts no longer live in the repo. The downloaded Frida devkit
  and per-build output (`config.h`, `agent.h`, `libcoyote.so`) are written to a
  user cache dir — `~/.cache/coyote/` — so the tool works installed read-only.
  Override with `$COYOTE_CACHE_DIR` (or `$XDG_CACHE_HOME`).
- The `frida-tools` dependency is pinned to `>=14.11.0`.

### Removed
- `requirements.txt`: the sole runtime dependency (`frida-tools`) is now
  declared in `pyproject.toml`.

## [2.1.1] - 2026-10-15

### Fixed
- `requirements.txt` declared exact-pinned versions (a `pip freeze` snapshot,
  incl. `frida==17.23.3`) that fail to install on other platforms/Python
  versions. It now declares only `frida-tools`; the devkit still pins itself to
  the installed frida version.

## [2.1.0] - 2026-10-15

### Added
- `profiles/generate_profiles.py`: generates complete, validator-passing profiles
  from `profile_database.json` (countries/carriers + real device profiles). Single
  profile via `--country`/`--carrier`/`--device`, or `--full-collection` to write
  one profile per carrier for every country under `<ISO>/` dirs. Location is
  randomized within ~10 km of the country's capital; identity, attribution,
  serial, build date and the full `system_prop` set are synthesized.
- `profiles/profile_database.json`: capital coordinates per country, consumed by
  the generator's location randomization.

## [2.0.0] - 2026-10-14

### Changed
- Build no longer uses Node/npm. The JS agent is built by concatenating the
  prebuilt `frida-java-bridge` bundle shipped with `frida-tools`
  (`bridges/java.js`) with `agent.js` — `agent.js` uses the bridge via the
  global it exposes instead of `require("frida-java-bridge")`.
- `--update-devkit` now fetches the devkit matching the installed `frida-tools`
  version (`frida.__version__`) instead of the latest release, keeping the
  embedded GumJS runtime and the bridge JS on the same Frida version.

### Removed
- `package.json`, `package-lock.json`, and the `frida-compile`/`frida-java-bridge`
  npm dependencies. Python deps are now in `requirements.txt` (`frida-tools`).

### Fixed
- JS agent recovers from the early-ART `getApi()` fault again: the VM exists from
  the zygote fork before the app's ART is initialized, so the readiness gate can
  pass too early — the bridge's cached `_apiError` is now cleared (defensively)
  so a later probe succeeds.

## [1.1.0] - 2026-10-11

### Added
- `-s` / `--serial <id>` flag to scope every ADB call (capture, ABI detection,
  devkit, deploy, reset) to a specific device via `ANDROID_SERIAL`, e.g.
  capture a profile from a physical phone and inject it into an emulator while
  both are connected.
- `/proc/self/maps` filtering that strips `libcoyote.so`, hiding the injected
  library from anti-tamper scans of mapped modules.
- `fstatat`/`newfstatat` hook so root/Magisk file-existence hiding also covers
  the stat path on bionic builds where `stat`/`lstat` are inline wrappers.
- Sensor fabrication: `SensorManager.getSensorList`/`getDefaultSensor` now
  return a fabricated set of `android.hardware.Sensor` objects (accelerometer,
  gyroscope, magnetometer, light, proximity, pressure) instead of only logging,
  defensive, falling back to recon-only if the ART build won't allow it.
- Location spoofing: new `location` profile section (lat/lon/accuracy/altitude);
  `LocationManager.getLastKnownLocation` returns the fix and `Geocoder` reverse-
  geocodes to the matching country. Coordinates come from an explicit `location`
  or, failing that, are derived from `telecom.country_iso`. `--capture` prefills
  `location` from the device's last-known fix (`dumpsys location`).

### Changed
- Native `getprop` spoofing now execs `echo <value>` directly (argv, no shell)
  in the `posix_spawn`/`posix_spawnp` paths, matching the `exec*` paths.
- JS agent waits for the ART VM via the JNI invocation API
  (`JNI_GetCreatedJavaVMs`) before probing the Java bridge, instead of poking
  frida-java-bridge's private `_apiError` field, version-proof, and it avoids
  the early-probe fault that made that poke necessary.

### Fixed
- Self-detection leak: in-memory spoof files (`/proc/cpuinfo`, `/proc/meminfo`,
  `/proc/uptime`, `/proc/mounts`, selinux) no longer use obvious `*_fake` memfd
  names that were visible under `/proc/self/fd`.
- Native `getprop` values containing a quote or shell metacharacter are no
  longer mishandled (and can't inject), since the shell is no longer involved.

## [1.0.0] - 2026-10-11

Initial release, an in-process Android device-spoofing agent for malware
analysis, with no `frida-server` footprint.

### Added
- **In-process injection** via a `wrap` + `LD_PRELOAD` hook; no daemon or Frida
  binaries left on the device.
- **In-process spawn-gating**: Java/ART hooks install on the main thread just
  before the first `Activity.onCreate`, so spoofing is live on first launch.
- **Native spoofing** (libgum/C): system properties, process execution, storage
  (`statfs`/`statvfs`), display/GPU (`ANativeWindow`, `glGetString`,
  `vkGetInstanceProcAddr`), `/proc/cpuinfo`, `/proc/meminfo`, `/proc/uptime`,
  anti-debug (`ptrace`), and root/Magisk hiding (su/Magisk files via
  `open`/`openat`/`access`/`faccessat`/`stat`/`lstat`, and `/proc/mounts`
  entries).
- **Managed spoofing** (GumJS + frida-java-bridge): display metrics, RAM,
  battery, telephony/SIM, time/locale, install referrer, USB state,
  `FLAG_SECURE`, network/VPN/proxy, dev/debug settings, SSL pinning, user-agent,
  package manager (app hiding + fake apps + installer source), keyguard,
  `android.os.Build` fields, `getprop` via `ProcessBuilder`, accounts, and Wi-Fi
  identity.
- **Profiles**: JSON device profiles compiled into a tailored `libcoyote.so`,
  with sections for telecom, bypass, device, specs, network, attribution,
  identity, and raw system properties.
- **Tooling**: `--capture` (build a complete profile from a real device, reading
  device facts and auto-generating the non-readable sections), `--recon` (log
  the probes a target makes), profile validation, silent release vs. verbose
  `--debug` builds, main-process-only JS (`--js-all-processes` to override),
  `--update-devkit`, `--reset-target`, and `--dry-run`.
- **Test harness**: the "Border Patrol" validation app covering every spoofing
  vector.

[Unreleased]: https://codeberg.org/davidjosearaujo/coyote/compare/v2.2.0...HEAD
[2.2.0]: https://codeberg.org/davidjosearaujo/coyote/compare/v2.1.1...v2.2.0
[2.1.1]: https://codeberg.org/davidjosearaujo/coyote/compare/v2.1.0...v2.1.1
[2.1.0]: https://codeberg.org/davidjosearaujo/coyote/compare/v2.0.0...v2.1.0
[2.0.0]: https://codeberg.org/davidjosearaujo/coyote/compare/v1.1.0...v2.0.0
[1.1.0]: https://codeberg.org/davidjosearaujo/coyote/compare/v1.0.0...v1.1.0
[1.0.0]: https://codeberg.org/davidjosearaujo/coyote/releases/tag/v1.0.0
