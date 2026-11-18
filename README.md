<p align="center">
  <img src="assets/coyote-logo.svg" alt="Coyote" width="128">
</p>

<h1 align="center">Coyote</h1>

<p align="center">
  <b>A stealthy, in-process device-spoofing agent for Android malware analysis.</b><br>
  <sub>Make an emulator or test device look like a clean phone — no <code>frida-server</code>, no device footprint.</sub>
</p>

<p align="center">
  <img alt="License: MIT" src="https://img.shields.io/badge/license-MIT-E0923A">
  <img alt="Python 3.10+" src="https://img.shields.io/badge/python-3.10%2B-3DDC84">
  <img alt="frida-server not required" src="https://img.shields.io/badge/frida--server-not%20required-3a3f47">
</p>

---

Coyote makes a real analysis device or emulator look like an arbitrary "clean"
physical phone to the app under test. It compiles a native shared library
(`libcoyote.so`) that is loaded into the target process and rewrites the answers
to the APIs apps use for environment/emulator detection, display metrics, RAM,
storage, battery, SIM/carrier, system properties, build fingerprint, network
state, installer source, and more, so that evasive apps which refuse to run on
an emulator will run, and can be observed.

Unlike a typical Frida setup, Coyote does **not** require `frida-server` running
on the device. There is no extra daemon process and no Frida binaries left on the
device. The agent lives entirely inside the target process, which keeps its
footprint minimal and hard to attribute.

Coyote installs as a Python package (`pipx install …`) that provides two
command-line tools: **`coyote`** — build a tailored spoofing library and inject
it into a target — and **`coyote-profiles`** — generate device profiles from a
built-in country/carrier/device database. See [Setup](#setup) to install and
[Usage](#usage) to run.

> ⚠️ **Scope / intent.** Coyote is a defensive tool for authorized malware
> analysis and security research on devices you own or are authorized to test.
> See [Legal & Ethical Use](#legal--ethical-use).

---

## Table of contents

- [What it does](#what-it-does)
- [How it works](#how-it-works)
  - [Injection: `wrap` + `LD_PRELOAD`](#injection-wrap--ld_preload)
  - [The two spoofing layers](#the-two-spoofing-layers)
  - [In-process spawn-gating](#in-process-spawn-gating)
  - [The build pipeline](#the-build-pipeline)
- [Requirements](#requirements)
- [Setup](#setup)
- [Usage](#usage)
  - [Command-line options](#command-line-options)
  - [Common workflows](#common-workflows)
- [Writing device profiles](#writing-device-profiles)
  - [Profile structure](#profile-structure)
  - [Section reference](#section-reference)
  - [Value typing rules](#value-typing-rules)
  - [`system_prop`: raw property overrides](#system_prop-raw-property-overrides)
- [What gets spoofed, and where](#what-gets-spoofed-and-where)
- [Troubleshooting](#troubleshooting)
- [Project layout](#project-layout)
- [Limitations](#limitations)
- [Legal & ethical use](#legal--ethical-use)

---

## What it does

Evasive Android malware often probes its environment and silently aborts (or
changes behavior) when it believes it is running in an emulator, a sandbox, or
an instrumented device. It looks for tell-tale signs: emulator display sizes,
suspiciously round RAM/storage values, no SIM, `ro.kernel.qemu=1`, `getprop`
values that scream "Android Studio AVD", a missing Play Store installer, USB
debugging flags, and so on.

Coyote intercepts those probes inside the target process and returns values from
a **device profile** you supply, a JSON description of the phone you want the
app to believe it is running on (e.g. a Google Pixel 8 Pro on Andorra Telecom).
The app's detection checks pass, and it proceeds to run so you can analyze it.

The result is a single `libcoyote.so` tailored to one profile and one target
package, injected without a persistent agent on the device.

---

## How it works

### Injection: `wrap` + `LD_PRELOAD`

Coyote does not patch the APK and does not run a server. It uses Android's
`wrap.<package>` system property, which tells Zygote to launch the app's process
through a wrapper command:

```
setprop wrap.<package> 'env LD_PRELOAD=/data/data/<package>/libcoyote.so'
```

When the app is next launched, Zygote starts it with `libcoyote.so` preloaded.
The library's constructor runs before the app's own code, on every process
Zygote forks for that package. This requires **root** (to place the `.so` inside
the app's private data directory with the correct SELinux context, and to set
the `wrap` property).

`libcoyote.so` is copied to `/data/data/<package>/libcoyote.so`, `chown`ed to the
app's UID, `chmod 755`, and labeled `u:object_r:app_data_file:s0` so the app is
allowed to map it under SELinux.

### The two spoofing layers

Coyote spoofs at two levels because detection happens at two levels. Everything
is driven by the compiled-in profile (the generated `config.h`).

**1. Native layer, `agent_src/puppyc.c` (libgum / C).**
Installs inline hooks (via Frida's GumJS devkit, used as a plain C library, no
scripts, no server) on libc/NDK functions that are reached very early, directly
from native code:

- **System properties**, `__system_property_get`, `__system_property_find`,
  `__system_property_read_callback`. Returns profile values for keys listed in
  `system_prop` (this is how `ro.*` read-only props are spoofed, since they can't
  be `setprop`'d).
- **Process execution**, `execve`, `execv`, `execvp`, `execvpe`, `posix_spawn`,
  `posix_spawnp`. Neutralizes root/su probes and serves spoofed `getprop <key>`
  output.
- **Anti-debug**, `ptrace`. Reports success for `PTRACE_TRACEME` without
  tracing, defeating ptrace-based debugger/instrumentation detection.
- **Storage**, `statfs`/`statfs64`/`statvfs`/`statvfs64` (total/free space).
- **Display / GPU**, `ANativeWindow_getWidth`/`getHeight`, `glGetString`,
  `vkGetInstanceProcAddr`.
- **Filesystem**, `open`/`openat`. Redirects reads of `/proc/cpuinfo` (←
  `device_profile.cpu_info`), `/proc/meminfo`, `/proc/uptime` (uptime = real
  boot clock + a multi-day offset), `/sys/fs/selinux/enforce`, etc. to in-memory
  spoofed content. Both `open` and `openat` are hooked so Java
  `FileReader`/`FileInputStream` reads are covered too.
- **Root / Magisk hiding**, `open`/`openat`/`access`/`faccessat`/`stat`/`lstat`
  report `ENOENT` for `su` binaries, Magisk/KernelSU/SuperSU paths, and
  Xposed/Frida artifacts, so `File.exists()`/`access()`/`stat()` existence probes
  come back negative. Reads of `/proc/mounts`, `/proc/self/mounts` and
  `/proc/self/mountinfo` are served with Magisk/`/data/adb` mount lines stripped
  out. (Magisk/root *apps* are also hidden at the package-manager layer.)

The native layer is applied from the library constructor, so it is active before
the app's first instruction.

**2. Managed layer, `agent_src/js/agent.js` (Frida GumJS / Java).**
A JavaScript agent, compiled into the library and executed by an embedded GumJS
runtime, uses `frida-java-bridge` to hook Android framework (Java/ART) methods
that native hooks can't reach cleanly:

- Display metrics (`Display`/`Resources.getDisplayMetrics`)
- RAM (`ActivityManager.getMemoryInfo`)
- Battery (`BatteryManager`, battery `Intent`/`Bundle` extras)
- Time/zone & locale (`Calendar`, `TimeZone`, `Locale`, `LocaleList`)
- Install referrer (`ReferrerDetails`, `Bundle`)
- USB state bypass (`Intent` extras)
- `FLAG_SECURE` stripping (`Window`)
- Telephony/carrier (`TelephonyManager`, resource `Configuration`)
- Dev/debug settings (`Settings.Global`/`Secure`/`System`, `Debug`, `VMDebug`)
- Network & VPN (`NetworkInfo`, `ConnectivityManager`, `NetworkCapabilities`,
  `NetworkInterface`)
- Proxy (`Proxy.getDefaultHost`)
- SSL pinning bypass (OkHttp, TrustKit, Conscrypt, Appcelerator, when present)
- User-Agent (`System.getProperty`, `WebView`, `HttpURLConnection`)
- Package manager (installer source, app hiding, fake installed apps, forced
  `hasSystemFeature`, install time)
- Keyguard (`KeyguardManager`)
- `android.os.Build` static fields (e.g. `Build.SERIAL`, which is hard-coded to
  `"unknown"` on modern Android and can only be fixed as a field)
- `getprop` via `ProcessBuilder`/`Runtime.exec` (rewritten to echo spoofed values)
- Accounts (`AccountManager.getAccounts`/`getAccountsByType` ← `identity`)
- Wi-Fi identity (`WifiInfo.getSSID`/`getBSSID`/`getMacAddress` ← `identity`)
- Sensors (`SensorManager.getSensorList`/`getDefaultSensor` return a fabricated
  set of `Sensor` objects; presence also forced via `hasSystemFeature`)
- Location (`LocationManager.getLastKnownLocation` + `Geocoder.getFromLocation`
  ← `location`, with the reverse-geocode country matching `telecom`)

### Process model

The `wrap` property applies to **every** process Zygote forks for the package,
so `libcoyote.so` loads into all of them. The two layers are scoped differently:

- The **native layer** is cheap, so it runs in **every** process.
- The **managed (JS/Java) layer** is heavy (a QuickJS runtime, `frida-java-bridge`,
  and a persistent event loop), so by default it runs **only in the main UI
  process**, where detection almost always happens. Auxiliary processes
  (named `<package>:<suffix>`) skip it and pay no JS cost.

The process is classified from `/proc/self/cmdline` (the bare package name is the
main process; a `:suffix` marks an auxiliary one); if it can't be determined,
Coyote runs the JS layer to avoid losing coverage. Pass `--js-all-processes` to
run the managed layer in every process instead.

### In-process spawn-gating

The hard problem: Java-layer hooks can only be installed once ART is up, but
detection often runs in the very first `Activity.onCreate`. A background thread
racing the app's startup loses that race (and instrumenting ART mid-bootstrap
from another thread deadlocks).

Coyote solves this **in-process**, without `frida-server`'s spawn gating:

1. As soon as the Java VM is usable, it arms **one** hook on
   `android.app.Instrumentation.callActivityOnCreate`, the framework method that
   runs, on the main thread, immediately before every activity's `onCreate`.
2. When that fires for the first activity, Coyote installs every
   **device-identity** hook family (Build, props, telephony, RAM, display,
   locale, sensors, package manager, FLAG_SECURE, battery, network/VPN, proxy,
   time, keyguard, USB, install referrer, accounts, Wi-Fi, location) **from the
   main thread**, before `onCreate` executes. Because the instrumentation
   happens on the main thread itself, there is no cross-thread ART suspension and
   no deadlock, and the whole install takes only tens of milliseconds.
3. `onCreate` then reads already-spoofed values for everything an app checks to
   decide whether it's in a sandbox.
4. Only the **network-time** families (`ssl` TLS-pinning bypass and `user_agent`)
   are **staged**: they take effect on the app's first TLS/HTTP traffic rather
   than at startup, so they are drained one per main-thread tick once the UI is
   up, off the launch critical path. The `CRITICAL` and `DEFERRED` lists at the
   top of `onJavaReady` in `agent.js` are the tuning knobs — move a block to
   `DEFERRED` only if a specific app deadlocks installing it synchronously.

> **Note:** the staging mechanism is now near-vestigial. With the whole
> device-identity surface installed synchronously before `onCreate`, only `ssl`
> and `user_agent` remain deferred, and those are naturally network-time anyway.
> Staging survives mainly as a safety valve: if a future hook deadlocks when
> installed synchronously on a particular app, moving it to `DEFERRED` is the
> escape hatch, without it the list would be empty.

The net effect is spawn-gating-quality timing (every device-identity spoof live
before the app's first checks) with zero added device footprint.

### The build pipeline

Running `coyote` with a profile performs, in order:

1. **`configure_adb_properties`**, `setprop`s the writable (`non-ro.*`) keys from
   `system_prop` over ADB. `ro.*` keys are skipped here and handled by the native
   property hook instead.
2. **`generate_c_header`**, turns the JSON profile into `config.h`
   (`#define`s + a `SYSTEM_PROP_OVERRIDES[]` table).
3. **`ensure_devkit`**, makes sure the Frida GumJS devkit for the target ABI is
   present in the cache (auto-downloads the version matching your installed
   `frida-tools` if missing — see below).
4. **`compile_js`**, concatenates the prebuilt `frida-java-bridge` bundle shipped
   by `frida-tools` (`bridges/java.js`) with the bundled `agent.js` and embeds
   the result as a C byte array in `agent.h`. No Node/npm — `agent.js` has
   no module imports and uses the bridge via the global it exposes.
5. **`compile_agent`**, compiles `coyote.c` + `puppyc.c` + `puppyjs.c` against
   the devkit static lib with the NDK Clang toolchain into `libcoyote.so`,
   then strips it.
6. **`launch_target_app`**, pushes the `.so`, fixes ownership/SELinux, sets the
   `wrap` property, force-stops the package, and relaunches it.

Generated artifacts (`config.h`, `agent.h`, `libcoyote.so`) and the downloaded
devkit are written under a user cache directory — `~/.cache/coyote/build/` and
`~/.cache/coyote/devkit/` (override with `$COYOTE_CACHE_DIR`, or `$XDG_CACHE_HOME`)
— so nothing is written inside the installed package.

The three C sources: `coyote.c` is the entry point (constructor → background
thread → `gum_init_embedded` → native hooks → GumJS runtime); `puppyc.c` holds
the native hooks; `puppyjs.c` builds the JS context (injecting the `config.h`
macros as JS globals) and runs the embedded agent.

---

## Requirements

- **A rooted Android device or emulator** (needs `su` for the SELinux-correct
  copy and the `wrap` property). The AVD "Google APIs" images with root, or a
  rooted test device, both work.
- **`adb`** on your `PATH`, with the device connected and authorized.
- **Android NDK**, pointed to by `$ANDROID_NDK_HOME` or `--ndk`.
- **Python 3.10+** (uses `str | None` typing).
- **`frida-tools`** (Python) — installed automatically as a dependency when you
  install Coyote; provides the prebuilt `frida-java-bridge` bundle and
  determines which devkit version to fetch. No Node.js/npm required.
- Internet access on first run (to fetch the Frida GumJS devkit), unless the
  devkit is already cached in `~/.cache/coyote/devkit/`.
- **A target whose ABI the device runs natively.** Coyote builds `libcoyote.so`
  for the target app's own ABI and injects via `wrap`+`LD_PRELOAD`. This works
  when the app runs as a *native* ABI of the device (including 32-bit apps on a
  64-bit device). It does **not** work for apps that only run through
  native-bridge translation — e.g. an **arm64-only app on an x86_64 emulator**:
  the translator never sees the preload, the app is forced into a non-native
  process, and its own native libraries fail to load (it hangs on the splash).
  Coyote detects this and stops with guidance rather than hanging. For such
  apps, use a **matching-ABI target** (an arm64 system image or a real arm64
  device). There is also an experimental `--nb-shim` path (see
  [Command-line options](#command-line-options)); it currently only gets the
  native layer running and needs SELinux permissive.

---

## Setup

Coyote installs as a Python package exposing two commands, `coyote` and
`coyote-profiles`. The recommended install is [`pipx`](https://pipx.pypa.io/),
which puts them on your `PATH` in an isolated environment:

```bash
# 1. Install from the repo (pipx isolates frida-tools in its own venv)
pipx install git+https://codeberg.org/davidjosearaujo/coyote.git
#   or from a local checkout:  git clone … coyote && pipx install ./coyote

# 2. Point to your NDK (or pass --ndk each run)
export ANDROID_NDK_HOME=/path/to/android-ndk

# 3. (Optional) pre-fetch the Frida GumJS devkit for your device ABI.
#    Auto-detected from the connected device if you omit the arch.
coyote --update-devkit            # auto-detect via ADB
coyote --update-devkit arm64      # or force one: arm | arm64 | x86 | x86_64
```

Prefer `pip`? `pip install .` (or `pip install -e .` for a hacking checkout)
works the same; the only runtime dependency is `frida-tools`.

The devkit (`frida-gumjs.h` + `libfrida-gumjs.a`, ~180 MB) is large and is
**not** shipped with the package. `--update-devkit` downloads the release
**matching your installed `frida-tools` version** (`frida.__version__`) from
GitHub and extracts it into `~/.cache/coyote/devkit/`. Pinning the devkit to
`frida-tools` keeps the embedded GumJS runtime and the bridge JS in lockstep —
if you upgrade `frida-tools`, re-run `--update-devkit` to match.

---

## Usage

The general form is:

```bash
coyote -p <profile.json> -t <target.package.name>
```

This builds a `libcoyote.so` tailored to `<profile.json>`, injects it into
`<target.package.name>`, and launches the app.

### Command-line options

| Option | Description |
| --- | --- |
| `-p`, `--profile <path>` | JSON device profile to compile in. Required to build/deploy. |
| `-t`, `--target <pkg>` | Target app package name (e.g. `com.example.app`). |
| `-s`, `--serial <id>` | ADB device serial (from `adb devices`) to scope **every** ADB call to. Use when a physical phone and an emulator are both connected. |
| `--ndk <path>` | Android NDK directory. Defaults to `$ANDROID_NDK_HOME`. |
| `--api-level <n>` | Target Android API level for NDK compilation (default `30`). |
| `--update-devkit [arch]` | Fetch the Frida GumJS devkit matching your `frida-tools` version (`auto`/`arm`/`arm64`/`x86`/`x86_64`), then exit. |
| `--reset-target` | Remove the `wrap` property, force-stop, and `pm clear` the target, undoing injection. |
| `--dry-run` | Print the generated `config.h` and all commands without compiling or changing the device. |
| `--debug` | Build with logging enabled (native `LOGI`/`LOGE` + JS `console`). **Default (release) builds are silent** and carry fewer identifying strings. |
| `--js-all-processes` | Run the JS/Java layer in **every** forked process of the package. Default runs it only in the main UI process (native hooks still apply everywhere). Use for a target that detects in a sub-process. |
| `--recon` | Build in **recon mode**: log every intercepted probe (property reads, `getprop`, `ptrace`, installer, accounts, Wi-Fi, sensors) under the `[CoyoteRecon]` / `[RECON]` tags so you can see what the target checks. Independent of `--debug`. |
| `--capture OUT.json` | Capture a **complete** profile from a connected **real** device via ADB, device facts read directly, non-readable sections (`bypass`, `attribution`, synthetic `identity` fields) auto-generated to plausible values, write it to `OUT.json`, then exit. It prints which fields were synthesized; review before use. |
| `--skip-adb` | Build the library but don't push writable `setprop`s to the device. |
| `--skip-deploy` | Compile only; don't push or launch (implies `--skip-adb`). |

### Common workflows

**Build, inject, and run against a target:**

```bash
coyote -p profiles/pixel7_us_tmobile.json -t com.suspicious.app
```

**Preview exactly what would happen (no device changes, no compile):**

```bash
coyote -p profiles/pixel7_us_tmobile.json -t com.suspicious.app --dry-run
```

**Compile the `.so` without touching the device (e.g. CI / offline build):**

```bash
coyote -p profiles/pixel7_us_tmobile.json -t com.suspicious.app --skip-deploy
# artifact: ~/.cache/coyote/build/libcoyote.so
```

**Remove Coyote from a target and reset its state:**

```bash
coyote -t com.suspicious.app --reset-target
```

**Capture a profile from a real device, then use it:**

```bash
coyote --capture my_device.json        # reads a connected real phone
# review my_device.json (esp. auto-generated fields), then:
coyote -p my_device.json -t com.suspicious.app
```

**Capture from a physical phone, inject into an emulator** (both connected),
scope each step to a device with `--serial` (from `adb devices`):

```bash
adb devices -l                                   # find the serials
coyote -s TA9990XYZ    --capture phone.json          # read the phone
coyote -s emulator-5554 -p phone.json -t com.suspicious.app   # build/inject into the emulator
```

The deploy step's ABI/devkit are resolved from the `--serial` target (the
emulator), so the `.so` matches where it runs.

`--capture` produces a **complete** profile (all sections) in two ways:

- **Read from the device**, `device_profile` (incl. `cpu_info`, GPU strings,
  keyboard), `specs` (display/RAM/storage/battery/swap), `telecom`
  (MCC/MNC/operator/ISO, locale, timezone, SIM state), `network` (local IPs,
  active transport), Wi-Fi SSID / account email where available, the last-known
  GPS fix (`location`, from `dumpsys location`), and the detection-relevant
  `system_prop` keys present on the device.
- **Auto-generated** (not readable off a device, so synthesized to plausible
  values), `bypass` (`flag_secure`), `attribution` (Play install source/
  referrer), `telecom.network_type`/`imei_prefix`, `specs.RAM_THRESHOLD`
  (derived), and `identity.wifi_bssid`/`wifi_mac` plus `wifi_ssid`/
  `account_email` when they can't be read. The command prints exactly which
  fields were synthesized so you can review them.
- **Derived at build time**, if no `location` is captured or set,
  `telecom.country_iso` picks a representative coordinate for that country.

**See what a target probes (recon mode):**

```bash
coyote -p profiles/pixel7_us_tmobile.json -t com.suspicious.app --recon
adb logcat -s "[CoyoteRecon]" "[PuppyJS]" | grep -E 'RECON|prop_get|ptrace'
```

**Build a verbose (debug) agent and watch it work:**

```bash
coyote -p profiles/pixel7_us_tmobile.json -t com.suspicious.app --debug
adb logcat -s "[Coyote]" "[PuppyC]" "[PuppyJS]"
```

Release builds (the default) are **silent**, they emit no log lines and omit the
agent's log/tag strings from the binary. Use `--debug` when you need to observe
the agent: you'll see the native hooks apply, then `gate armed …`, `gate fired
…`, and `All hook blocks installed` before the app's UI appears. In a debug run
you can also confirm the process model: the **main** process prints the full
`JSAgent` hook log, while **auxiliary** processes print `Auxiliary process;
skipping JS layer.` and nothing more.

You should see the native hooks apply, then `gate armed …`, `gate fired …`, and
`All hook blocks installed` before the app's UI appears.

---

## Writing device profiles

A profile is a single JSON file describing the phone you want to impersonate.
Each top-level section maps to a group of spoofed values. See
`profiles/pixel7_us_tmobile.json` for a complete working example; the
`test/.../test_profile.json` used by the bundled test app is another.

Every key you include is turned into a compile-time macro and consumed by the
hooks. **Keys you omit are simply not spoofed**, the real device value shows
through. Start from an existing profile and change what you need.

You can also **generate** complete profiles from a country/carrier/device
database instead of writing them by hand with the `coyote-profiles` command
(see [its README](src/coyote/profiles/README.md)), e.g.
`coyote-profiles --country US --device google-pixel-8-pro -o my.json`
(location is randomized within ~10 km of the country's capital). `--full-collection`
builds one profile per carrier for every country.

### Profile structure

```jsonc
{
  "telecom":        { ... },   // SIM / carrier / network identity
  "bypass":         { ... },   // feature toggles (e.g. FLAG_SECURE)
  "device_profile": { ... },   // Build.* identity & hardware metadata
  "specs":          { ... },   // display, memory, storage, battery
  "network":        { ... },   // connectivity type & local IPs
  "attribution":    { ... },   // install referrer / attribution
  "system_prop":    { ... },   // raw Android system properties (getprop)
  "identity":       { ... },   // Wi-Fi and account identity
  "location":       { ... }    // GPS coordinates
}
```

### Section reference

Each section is emitted with a macro prefix; the table shows representative keys
drawn from the example profiles.

**`telecom` → `TELECOM_*`**, SIM and carrier identity.

| Key | Example | Meaning |
| --- | --- | --- |
| `mcc` / `mnc` | `"213"` / `"03"` | Mobile country/network codes |
| `operator_name` | `"Andorra Telecom / Mobiland"` | SIM/network operator name |
| `country_iso` | `"ad"` | SIM country ISO |
| `language` | `"ca"` | Carrier language |
| `phone_code` | `"376"` | Dialing prefix used for a fake MSISDN |
| `sim_state_num` | `5` | `TelephonyManager` SIM state (5 = READY) |
| `imei_prefix` | `"35"` | Prefix for a generated IMEI |
| `network_type` | `"LTE"` | Radio network type |
| `timezone_id` | `"Europe/Andorra"` | IANA timezone |

**`bypass` → `BYPASS_*`**, boolean feature toggles.

| Key | Example | Meaning |
| --- | --- | --- |
| `flag_secure` | `1` | Strip `FLAG_SECURE` so screenshots/recording work |

**`device_profile` → `DEVICE_*`**, `android.os.Build` identity and hardware
metadata: `model`, `manufacturer`, `brand`, `device`, `board`, `product`,
`hardware`, `fingerprint`, `bootloader`, `serial`, `display`/`display_id`,
`security_patch`, `soc_model`, `gl_vendor`/`gl_renderer`/`gl_version`,
`keyboard`, `cpu_info`, and more. These feed the `Build.*` field spoofing and
GPU/CPU strings. (`cpu_info` is emitted as a C raw-string literal, so multi-line
`/proc/cpuinfo` text is fine.)

**`specs` → `SPECS_*`**, physical hardware metrics. Keys are UPPERCASE here.

| Key | Example | Meaning |
| --- | --- | --- |
| `WIDTH` / `HEIGHT` | `1440` / `3120` | Display pixels |
| `DENSITY` | `"2.75f"` | Logical density |
| `DENSITY_DPI` | `440` | Density bucket |
| `XDPI` / `YDPI` | `"440.0f"` | Physical DPI |
| `TOTAL_SPACE` / `FREE_SPACE` | `549755813888` | Storage bytes |
| `TOTAL_RAM` / `AVAIL_RAM` | `11886625832` | RAM bytes |
| `RAM_THRESHOLD` | `594331291` | Low-memory threshold |
| `BATTERY_LEVEL` | `50` | Battery percentage |

**`network` → `NETWORK_*`**, connectivity.

| Key | Example | Meaning |
| --- | --- | --- |
| `net_type` | `0` | Active transport (`0` = mobile, `1` = Wi-Fi) |
| `mobile_data_local_ip` | `"10.23.45.67"` | Fake mobile interface IP |
| `wifi_local_ip` | `"192.168.1.211"` | Fake Wi-Fi interface IP |

**`attribution` → `ATTRIBUTION_*`**, install/attribution data.

| Key | Example | Meaning |
| --- | --- | --- |
| `install_referrer` | `"utm_source=apps.facebook.com&..."` | Play install referrer |
| `install_package` | `"com.android.vending"` | Expected installer package |
| `campaign`, `af_status`, `facebook_app_id` | … | Attribution fields |

**`identity` → `IDENTITY_*`**, Wi-Fi and account identity (a "lived-in" device).
All keys optional; omit one and that hook stays off.

| Key | Example | Meaning |
| --- | --- | --- |
| `wifi_ssid` | `"HomeWiFi_5G"` | `WifiInfo.getSSID` |
| `wifi_bssid` | `"a4:2b:8c:11:22:33"` | `WifiInfo.getBSSID` |
| `wifi_mac` | `"a4:2b:8c:44:55:66"` | `WifiInfo.getMacAddress` |
| `account_email` | `"jane.doe@gmail.com"` | Fake account name from `AccountManager` |
| `account_type` | `"com.google"` | Account type (defaults to `com.google`) |

**`location` → `LOCATION_*`**, a GPS fix. If omitted but `telecom.country_iso`
is known, Coyote derives a representative coordinate for that country so the fix
matches the SIM country automatically.

| Key | Example | Meaning |
| --- | --- | --- |
| `latitude` | `42.5063` | degrees (double) |
| `longitude` | `1.5218` | degrees (double) |
| `accuracy` | `18.0` | meters, optional |
| `altitude` | `1023.0` | meters, optional |

**`system_prop`**, see below (handled specially).

### Value typing rules

`config.h` generation infers the C type from the JSON value, so **use the right
JSON type**:

- **String** → quoted C string. Use this for IDs, names, ISO codes, etc.
- **Float** (JSON number with a decimal, or a string like `"2.75f"`) → a C
  `float` literal with an `f` suffix. Density/DPI values are commonly written as
  strings with a trailing `f`.
- **Integer** → a plain `int`, **except** the size keys `TOTAL_SPACE`,
  `FREE_SPACE`, `TOTAL_RAM`, `AVAIL_RAM`, and `RAM_THRESHOLD`, which are emitted
  as `unsigned long long` (`ULL`), always give these as plain numbers (bytes),
  not strings.
- **`cpu_info`** is special-cased into a raw string literal so it can contain
  quotes and newlines.

Run with `--dry-run` to see the exact `config.h` that your profile produces
before building.

### `system_prop`: raw property overrides

`system_prop` is a flat map of Android property keys to spoofed values, e.g.:

```jsonc
"system_prop": {
  "persist.sys.timezone": "Europe/Andorra",
  "ro.product.model":     "Pixel 8 Pro",
  "ro.kernel.qemu":       "",
  "ro.serialno":          "7PZGGNTD92N",
  "gsm.operator.numeric": "21303"
}
```

These are handled in two ways:

- **Writable properties** (not starting with `ro.`) are applied live over ADB
  with `setprop` before launch.
- **Read-only properties** (`ro.*`) cannot be `setprop`'d, so they are compiled
  into a `SYSTEM_PROP_OVERRIDES[]` table and served by the **native**
  `__system_property_get` / `__system_property_read_callback` hooks inside the
  process. This covers both direct property reads and `getprop` shell calls.

This is where you neutralize the classic emulator tells, set `ro.kernel.qemu`,
`ro.boot.qemu`, `ro.hardware`, the `qemu.*` keys, `ro.build.fingerprint`,
`ro.serialno`, SELinux/verified-boot flags, etc. to realistic values. The example
profiles cover a large, practical set (100+ keys) you can use as a baseline.

---

## What gets spoofed, and where

| Detection vector | Layer | Key(s) / hook |
| --- | --- | --- |
| Display size / density | Native + JS | `ANativeWindow_*`, `Display`/`Resources.getDisplayMetrics` ← `specs` |
| RAM | JS | `ActivityManager.getMemoryInfo` ← `specs` |
| Storage | Native | `statfs`/`statvfs*` ← `specs` |
| Battery | JS | `BatteryManager`, battery intent extras ← `specs` |
| SIM / carrier | JS | `TelephonyManager` ← `telecom` |
| Build identity | JS + Native | `Build.*` fields + `ro.product.*`/`ro.build.*` props ← `device_profile`, `system_prop` |
| System properties / `getprop` | Native (+ JS for `ProcessBuilder`) | property hooks + exec rewrite ← `system_prop` |
| Root / su probes | Native | `exec*`/`posix_spawn*` neutralization |
| Network / VPN / proxy | JS | `ConnectivityManager`, `NetworkCapabilities`, `Proxy` ← `network` |
| Install source | JS | `ApplicationPackageManager.getInstallerPackageName`, `InstallSourceInfo` ← `attribution` |
| Install referrer | JS | `ReferrerDetails`, `Bundle` ← `attribution` |
| USB debugging state | JS | USB `Intent` extras |
| `FLAG_SECURE` | JS | `Window.addFlags`/`setFlags` ← `bypass` |
| Locale / timezone | JS + Native | `Locale`/`TimeZone` + `persist.sys.*` props |
| SSL pinning | JS | OkHttp / TrustKit / Conscrypt / Appcelerator (if present) |
| Keyguard | JS | `KeyguardManager` |
| Anti-debug | Native | `ptrace(PTRACE_TRACEME)` neutralized |
| Root / Magisk | Native + JS | `su`/Magisk paths hidden from `open`/`access`/`stat`; Magisk mounts stripped from `/proc/mounts`; root apps hidden from `PackageManager` |
| CPU fingerprint | Native | `/proc/cpuinfo` ← `device_profile.cpu_info` |
| Uptime | Native | `/proc/uptime` → real boot clock + multi-day offset |
| Accounts | JS | `AccountManager` ← `identity` |
| Wi-Fi identity | JS | `WifiInfo` SSID/BSSID/MAC ← `identity` |
| Sensors | JS | fabricated `Sensor` objects via `SensorManager`; presence via `hasSystemFeature` |
| Location | JS | `LocationManager` + `Geocoder` ← `location` (country-derived fallback) |

---

## Troubleshooting

- **`[!] Error: Invalid NDK directory`**, set `$ANDROID_NDK_HOME` or pass
  `--ndk /path/to/ndk`.
- **Devkit missing / download fails**, run `coyote --update-devkit`
  (with the device connected for auto ABI detection), or specify the arch.
- **`frida-tools is not installed`**, reinstall Coyote (`pipx install …` /
  `pip install .`) so its `frida-tools` dependency is present. The build reads
  `frida-java-bridge` from `frida-tools`' `bridges/java.js`.
- **Devkit download 404 / bridge calls a missing runtime API**, your devkit and
  `frida-tools` versions drifted. Re-run `--update-devkit` to fetch the devkit
  matching the installed `frida-tools`.
- **Permission / SELinux errors during deploy**, the device must be rooted; the
  copy step uses `su 0`. Verify `adb shell su 0 id` works.
- **App still detects the emulator**, build with `--debug` and check `logcat`
  for the hook logs (release builds are silent). If you see `gate armed` but never
  `gate fired`, the app may not go through a standard activity launch; if a
  specific value is wrong, confirm the matching profile key (and run `--dry-run`
  to verify it made it into `config.h`). If the detection runs in a sub-process,
  rebuild with `--js-all-processes`.
- **Multiple `[Coyote] Coyote loaded` lines with different PIDs**, expected. The
  `wrap` property applies to every process Zygote forks for the package; each one
  loads the library and applies the native hooks. By default only the main
  process also runs the JS layer (auxiliary processes log `Auxiliary process;
  skipping JS layer.` in a `--debug` build).
- **Wrong architecture**, the devkit ABI must match the device. Re-fetch with
  `--update-devkit <arch>` and rebuild.

---

## Project layout

```
coyote/
├── pyproject.toml          # Packaging: deps (frida-tools), `coyote` + `coyote-profiles` entry points
├── src/coyote/             # The installable package
│   ├── cli.py              # `coyote`: profile → config.h → build → inject
│   ├── coyote_profiles.py  # `coyote-profiles`: generate full profiles (single or --full-collection)
│   ├── paths.py            # Package-data vs. user-cache path resolution
│   ├── agent_src/
│   │   ├── coyote.c        # Library entry point (constructor, threads, gum init)
│   │   ├── puppyc.c        # Native (libgum/C) hooks
│   │   ├── puppyjs.c       # Embedded GumJS runtime + macro injection + agent loader
│   │   └── js/agent.js     # Managed (Java/ART) hooks + in-process spawn-gating
│   └── profiles/           # Shipped data
│       ├── pixel7_us_tmobile.json   # hand-written example
│       └── profile_database.json    # countries/carriers/devices + capital coords
└── test/                   # "Border Patrol" validation app (~314 detection checks)

~/.cache/coyote/            # Writable, created at runtime (override: $COYOTE_CACHE_DIR)
├── devkit/                 # Frida GumJS devkit (fetched via --update-devkit)
└── build/                  # Generated: config.h, _agent.js, agent.h, libcoyote.so
```

---

## Limitations

- **Root required.** Injection relies on the `wrap` property and placing the
  library in the app's private data dir with the right SELinux label.
- **One profile + one target per build.** The profile and package are compiled
  in; rebuild to change either.
- **Per-ABI builds.** The native library must match the device architecture.
- **Framework-version sensitivity.** Managed hooks target specific Android
  classes/methods; very new or heavily customized ROMs may move or rename some,
  in which case that hook logs an error and is skipped (others continue).
- **Not an anti-anti-Frida bypass per se.** Coyote's goal is environment
  spoofing. It avoids a `frida-server` footprint, but a determined app can still
  look for in-process instrumentation; hardening against that is out of scope.

---

## Legal & ethical use

Coyote is intended for **authorized** malware analysis, security research, and
quality assurance on devices and applications you own or have explicit permission
to test. Do not use it to bypass protections on software you are not authorized
to analyze, to facilitate fraud, or to evade controls in production systems. You
are responsible for complying with all applicable laws and agreements.

See `LICENSE` for distribution terms.
