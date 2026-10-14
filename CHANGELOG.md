# Changelog

All notable changes to Coyote are documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

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

[Unreleased]: https://codeberg.org/davidjosearaujo/coyote/compare/v2.0.0...HEAD
[2.0.0]: https://codeberg.org/davidjosearaujo/coyote/compare/v1.1.0...v2.0.0
[1.1.0]: https://codeberg.org/davidjosearaujo/coyote/compare/v1.0.0...v1.1.0
[1.0.0]: https://codeberg.org/davidjosearaujo/coyote/releases/tag/v1.0.0
