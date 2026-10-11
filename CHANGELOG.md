# Changelog

All notable changes to Coyote are documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

### Added
- `-s` / `--serial <id>` flag to scope every ADB call (capture, ABI detection,
  devkit, deploy, reset) to a specific device via `ANDROID_SERIAL` — e.g.
  capture a profile from a physical phone and inject it into an emulator while
  both are connected.

## [1.0.0] - 2026-10-11

Initial release — an in-process Android device-spoofing agent for malware
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
