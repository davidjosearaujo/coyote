# Changelog

All notable changes to Coyote are documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

### Added
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
  channel. The agent loads and the native (PuppyC) hooks run inside the guest,
  but frida-gum is currently unstable under translation (a secondary thread may
  crash before the JS/Java layer comes up), and it requires SELinux permissive.
  Off by default.

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
- JS agent hook install is now **staged** to stop real-world apps from hanging
  on the splash screen. Previously all ~70 Java method replacements were
  installed in one synchronous burst inside `callActivityOnCreate`; each
  replacement triggers an ART deoptimization that suspends every thread in the
  process. On a trivial test app that was only slow, but on real apps doing
  heavy work on background threads during the splash the back-to-back
  all-thread suspends caused ANRs/deadlocks, so they never got past the splash.
  `onJavaReady` now installs only the detection-critical hook families
  (`build`, `getprop`, `debug_settings`, `telephony`, `ram`, `display`,
  `locale_tz`, `sensors`, `package_manager`) synchronously before the first
  activity's `onCreate`, then drains the rest one family per main-thread tick
  (via `Java.scheduleOnMainThread`) once the splash is already up, spreading the
  remaining deopts across frames off the critical path. The `CRITICAL` /
  `DEFERRED` lists in `agent.js` are the tuning knobs; deferred TLS-pinning
  bypass (`ssl`) installs on the first idle tick, so move it to `CRITICAL` if a
  target pins during its splash network calls.

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
