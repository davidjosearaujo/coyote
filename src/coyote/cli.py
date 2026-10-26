#!/usr/bin/env python3
import argparse
import json
import tarfile
import os
import re
import subprocess
import platform
import sys
import urllib.request
from pathlib import Path

from .paths import AGENT_SRC, BUILD_DIR, DEVKIT_DIR

ABI_MAP = {
    "arm64-v8a": {"compiler": "aarch64-linux-android", "devkit": "arm64"},
    "armeabi-v7a": {"compiler": "armv7a-linux-androideabi", "devkit": "arm"},
    "x86_64": {"compiler": "x86_64-linux-android", "devkit": "x86_64"},
    "x86": {"compiler": "i686-linux-android", "devkit": "x86"},
}

# Representative city coordinates per ISO-3166 alpha-2 country code, used to
# derive a plausible GPS fix that matches a profile's telecom country when no
# explicit `location` is given. (lat, lon)
COUNTRY_COORDS = {
    "ad": (42.5063, 1.5218), "us": (40.7128, -74.0060), "gb": (51.5074, -0.1278),
    "ca": (43.6532, -79.3832), "au": (-33.8688, 151.2093), "de": (52.5200, 13.4050),
    "fr": (48.8566, 2.3522), "es": (40.4168, -3.7038), "pt": (38.7223, -9.1393),
    "it": (41.9028, 12.4964), "nl": (52.3676, 4.9041), "be": (50.8503, 4.3517),
    "ch": (47.3769, 8.5417), "at": (48.2082, 16.3738), "ie": (53.3498, -6.2603),
    "se": (59.3293, 18.0686), "no": (59.9139, 10.7522), "dk": (55.6761, 12.5683),
    "fi": (60.1699, 24.9384), "pl": (52.2297, 21.0122), "cz": (50.0755, 14.4378),
    "ru": (55.7558, 37.6173), "ua": (50.4501, 30.5234), "tr": (41.0082, 28.9784),
    "gr": (37.9838, 23.7275), "br": (-23.5505, -46.6333), "mx": (19.4326, -99.1332),
    "ar": (-34.6037, -58.3816), "cl": (-33.4489, -70.6693), "co": (4.7110, -74.0721),
    "in": (28.6139, 77.2090), "cn": (39.9042, 116.4074), "jp": (35.6762, 139.6503),
    "kr": (37.5665, 126.9780), "id": (-6.2088, 106.8456), "th": (13.7563, 100.5018),
    "vn": (21.0278, 105.8342), "ph": (14.5995, 120.9842), "my": (3.1390, 101.6869),
    "sg": (1.3521, 103.8198), "ae": (25.2048, 55.2708), "sa": (24.7136, 46.6753),
    "il": (32.0853, 34.7818), "za": (-26.2041, 28.0473), "ng": (6.5244, 3.3792),
    "eg": (30.0444, 31.2357), "nz": (-36.8485, 174.7633),
}

def detect_device_abi() -> dict:
    """Queries target device via ADB, defaulting to the host's architecture if unavailable."""
    try:
        cmd = ["adb", "shell", "getprop", "ro.product.cpu.abi"]
        raw_abi = subprocess.check_output(cmd, stderr=subprocess.DEVNULL).decode().strip()
        if raw_abi in ABI_MAP:
            return ABI_MAP[raw_abi]
        print(f"[!] Unknown ABI '{raw_abi}'. Defaulting to host architecture.")
    except Exception:
        print("[!] ADB query failed. Defaulting compiler to host architecture.")
    
    machine = platform.machine().lower()
    if machine in ["x86_64", "amd64"]:
        return ABI_MAP["x86_64"]
    elif machine in ["aarch64", "arm64"]:
        return ABI_MAP["arm64-v8a"]
    elif machine in ["i386", "i686", "x86"]:
        return ABI_MAP["x86"]
    
    # Safe fallback
    return ABI_MAP["x86_64"]

# Broad CPU family per ABI. An app whose ABI is a different family than the
# device's primary ABI only runs through a native-bridge translator (e.g.
# arm64 apps on an x86_64 emulator via libndk_translation), which the plain
# wrap+LD_PRELOAD inject can't cross — see resolve_target_abi().
ARCH_FAMILY = {
    "arm64-v8a": "arm", "armeabi-v7a": "arm",
    "x86_64": "x86", "x86": "x86",
}

# Translated-guest app_process launched by the native-bridge shim, keyed by the
# guest app's primary ABI (used only on the experimental --nb-shim path).
GUEST_APP_PROCESS = {
    "arm64-v8a": "/system/bin/arm64/app_process64",
    "armeabi-v7a": "/system/bin/arm/app_process32",
}

def detect_app_abi(package: str) -> str | None:
    """The installed target app's primaryCpuAbi (what ABI its process runs as),
    or None if unknown / not installed. This is what the agent must match, and
    it may differ from the device's primary ABI (then the app is translated)."""
    try:
        out = subprocess.check_output(
            ["adb", "shell", "dumpsys", "package", package],
            stderr=subprocess.DEVNULL,
        ).decode(errors="replace")
    except Exception:
        return None
    m = re.search(r"primaryCpuAbi=(\S+)", out)
    if m and m.group(1) not in ("null", ""):
        return m.group(1).strip()
    return None

def detect_device_primary_abi() -> str | None:
    try:
        abi = subprocess.check_output(
            ["adb", "shell", "getprop", "ro.product.cpu.abi"],
            stderr=subprocess.DEVNULL,
        ).decode().strip()
        return abi or None
    except Exception:
        return None

def resolve_target_abi(package: str) -> dict:
    """Decide which ABI to build the agent for, and whether the target app only
    runs under native-bridge translation (which wrap injection can't reach).

    Returns {app_abi, device_abi, build_abi, translated}. On any detection
    failure it falls back to the device's primary ABI with translated=False, so
    behaviour is unchanged when ADB/dumpsys can't answer."""
    app_abi = detect_app_abi(package) if package else None
    device_abi = detect_device_primary_abi()

    if not app_abi or app_abi not in ABI_MAP:
        # Unknown app ABI: keep the historical behaviour (build for the device).
        build_abi = device_abi if device_abi in ABI_MAP else None
        return {"app_abi": app_abi, "device_abi": device_abi,
                "build_abi": build_abi, "translated": False}

    translated = bool(
        device_abi
        and ARCH_FAMILY.get(app_abi) != ARCH_FAMILY.get(device_abi)
    )
    # Build for the ABI the app's process actually runs as, not the device's.
    return {"app_abi": app_abi, "device_abi": device_abi,
            "build_abi": app_abi, "translated": translated}

class Coyote:

    def __init__(
        self,
        profile_path: str | None = None,
        ndk_path: str | None = None,
        api_level: int = 30,
        dry_run: bool = False,
        package_name: str = "test.target.app",
        debug: bool = False,
        js_all_processes: bool = False,
        recon: bool = False,
    ):
        self.dry_run = dry_run
        self.debug = debug
        self.js_all_processes = js_all_processes
        self.recon = recon
        self.api_level = api_level
        self.package_name = package_name
        self.ndk_path = Path(ndk_path) if ndk_path else None
        
        self.profile = self._load_profile(profile_path) if profile_path else {}

        if not dry_run and profile_path:
            BUILD_DIR.mkdir(parents=True, exist_ok=True)

    def _load_profile(self, path: str) -> dict:
        profile_file = Path(path)
        if not profile_file.is_file():
            print(f"[!] Error: Profile file '{path}' not found.")
            sys.exit(1)
        with open(profile_file, "r") as f:
            return json.load(f)

    def validate_profile(self) -> bool:
        """Validates the loaded profile: section shapes, value types, and a few
        cross-consistency checks. Hard errors abort the build; soft issues warn."""
        print("\n[+] Validating profile...")

        KNOWN_SECTIONS = {
            "telecom", "bypass", "device_profile", "specs",
            "network", "attribution", "identity", "location", "system_prop",
        }
        # Keys that must be plain integers (emitted as unsigned long long bytes).
        SIZE_KEYS = {"TOTAL_SPACE", "FREE_SPACE", "TOTAL_RAM", "AVAIL_RAM", "RAM_THRESHOLD"}

        errors: list[str] = []
        warnings: list[str] = []

        if not isinstance(self.profile, dict) or not self.profile:
            print("[!] Profile is empty or not a JSON object.")
            sys.exit(1)

        for section in self.profile:
            if section not in KNOWN_SECTIONS:
                warnings.append(f"Unknown top-level section '{section}' (ignored by the build).")

        for section in KNOWN_SECTIONS:
            val = self.profile.get(section)
            if val is not None and not isinstance(val, dict):
                errors.append(f"Section '{section}' must be an object, got {type(val).__name__}.")

        specs = self.profile.get("specs", {})
        if isinstance(specs, dict):
            for k in SIZE_KEYS:
                if k in specs and not isinstance(specs[k], int):
                    errors.append(f"specs.{k} must be an integer (bytes), got {type(specs[k]).__name__}.")
            for k in ("WIDTH", "HEIGHT", "DENSITY_DPI", "BATTERY_LEVEL"):
                if k in specs and not isinstance(specs[k], int):
                    errors.append(f"specs.{k} must be an integer, got {type(specs[k]).__name__}.")

        sysp = self.profile.get("system_prop", {})
        if isinstance(sysp, dict):
            for k, v in sysp.items():
                if not isinstance(v, str):
                    errors.append(f"system_prop['{k}'] must be a string, got {type(v).__name__}.")

        # Cross-consistency: MCC+MNC vs the gsm.operator.numeric property.
        tel = self.profile.get("telecom", {})
        if isinstance(tel, dict) and isinstance(sysp, dict):
            if "mcc" in tel and "mnc" in tel and "gsm.operator.numeric" in sysp:
                expected = f"{tel['mcc']}{tel['mnc']}"
                if sysp["gsm.operator.numeric"] != expected:
                    warnings.append(
                        f"telecom mcc+mnc ('{expected}') != system_prop['gsm.operator.numeric'] "
                        f"('{sysp['gsm.operator.numeric']}'), a detector comparing them would notice."
                    )

        for w in warnings:
            print(f"    [warn] {w}")
        if errors:
            for e in errors:
                print(f"    [error] {e}")
            print(f"[!] Profile validation failed with {len(errors)} error(s).")
            sys.exit(1)

        print(f"    [+] Profile OK ({len(warnings)} warning(s)).")
        return True

    def _exec(self, cmd: list, desc: str = "") -> str:
        """Executes shell commands or prints them if dry_run is set."""
        cmd_str = " ".join(cmd)
        if self.dry_run:
            print(f"  [DRY-RUN] {cmd_str}")
            return ""

        res = subprocess.run(cmd, capture_output=True, text=True)
        if res.returncode != 0:
            print(f"[!] Command failed ({desc}):\n{res.stderr.strip()}")
            if not self.dry_run:
                sys.exit(1)
        return res.stdout.strip()

    def configure_adb_properties(self):
        """Sets standard ADB system properties defined in the profile."""
        print(
            f"\n[+] Applying ADB properties for: {self.profile.get('device_profile', {}).get('marketing_name', 'Custom Profile')}"
        )
        sys_props = self.profile.get("system_prop", {})
        for prop, val in sys_props.items():
            if prop.startswith("ro."):
                print(f"    - [SKIP] {prop} (Read-only property; handled by memory hook)")
                continue

            print(f"    - setprop {prop} -> {val}")
            self._exec(
                ["adb", "shell", "setprop", prop, f"'{val}'"],
                desc=f"setprop {prop}",
            )

    def _sanitize_macro_key(self, prop_key: str) -> str:
        """Converts property keys like 'ro.build.display.id' to 'SYS_PROP_RO_BUILD_DISPLAY_ID'."""
        sanitized = re.sub(r"[^a-zA-Z0-9_]", "_", prop_key).upper()
        return f"SYS_PROP_{sanitized}"

    def _resolve_location(self):
        """Fill in a GPS fix when the profile has none: if there's no explicit
        `location` (with lat/lon) but telecom.country_iso maps to a known city,
        derive coordinates from it so the fix matches the SIM country."""
        loc = self.profile.get("location") or {}
        if "latitude" in loc and "longitude" in loc:
            return  # explicit coordinates win
        iso = (self.profile.get("telecom", {}) or {}).get("country_iso", "").lower()
        coords = COUNTRY_COORDS.get(iso)
        if coords:
            lat, lon = coords
            self.profile["location"] = {"latitude": lat, "longitude": lon, "accuracy": 20.0}
            print(f"    [+] Derived location from telecom.country_iso='{iso}': {lat}, {lon}")

    def generate_c_header(self):
        """Dynamically generates config.h from the JSON profile without a template."""
        output_file = BUILD_DIR / "config.h"
        print(f"\n[+] Generating {output_file} ...")
        self._resolve_location()

        def format_value(key: str, val) -> str:
            """Infers the correct C-type formatting for the macro value."""
            if key == "CPU_INFO":
                return f'R"raw_cpu({val})raw_cpu"'
            if isinstance(val, str):
                return f'"{val}"'
            if isinstance(val, float):
                # GPS coordinates need double precision, emit as a double (no
                # 'f' suffix) so latitude/longitude aren't truncated to float.
                if key in ("LATITUDE", "LONGITUDE", "ACCURACY", "ALTITUDE"):
                    return repr(val)
                return f"{val}f"
            if isinstance(val, int):
                if key in ["TOTAL_SPACE", "FREE_SPACE", "TOTAL_RAM", "AVAIL_RAM", "RAM_THRESHOLD"]:
                    return f"{val}ULL"
                return str(val)
            return str(val)

        lines = [
            "#ifndef COYOTE_CONFIG_H",
            "#define COYOTE_CONFIG_H",
            ""
        ]

        sections = [
            ("1. Telecom Settings", "telecom", "TELECOM_"),
            ("2. Bypass Settings", "bypass", "BYPASS_"),
            ("3. Device Identification & Build Metadata", "device_profile", "DEVICE_"),
            ("4. Hardware Metrics & Memory Space", "specs", "SPECS_"),
            ("5. Network Configuration", "network", "NETWORK_"),
            ("6. Attribution & Install Tracking", "attribution", "ATTRIBUTION_"),
            ("7. Identity & Accounts (Wi-Fi, accounts)", "identity", "IDENTITY_"),
            ("8. Location (GPS fix)", "location", "LOCATION_"),
        ]

        macro_count = 0

        for title, profile_key, prefix in sections:
            lines.append(f"// {title}")
            for k, v in self.profile.get(profile_key, {}).items():
                macro_name = f"{prefix}{k.upper()}"
                val_str = format_value(k.upper(), v)
                lines.append(f"#define {macro_name:<31} {val_str}")
                macro_count += 1
            lines.append("")

        sys_props = self.profile.get("system_prop", {})
        if len(sys_props) > 0:
            lines.append("// 6. System Properties Interception")
            prop_overrides = []
            for raw_key, val in sys_props.items():
                macro_name = self._sanitize_macro_key(raw_key)
                prop_overrides.append((raw_key, macro_name))
                lines.append(f"#define {macro_name:<60} \"{val}\"")
                macro_count += 1
                
            lines.append("typedef struct {")
            lines.append("    const char *key;")
            lines.append("    const char *value;")
            lines.append("} prop_override_t;\n")

            lines.append("static const prop_override_t SYSTEM_PROP_OVERRIDES[] = {")
            # Emit sorted by key so the native lookup can use bsearch(). Property
            # keys are ASCII, so Python's string ordering matches C strcmp byte
            # ordering. The {NULL, NULL} sentinel stays last and is excluded from
            # the searched range via SYSTEM_PROP_OVERRIDES_COUNT.
            for raw_key, macro_name in sorted(prop_overrides, key=lambda x: x[0]):
                lines.append(f'    {{"{raw_key}", {macro_name}}},')
            lines.append("    {NULL, NULL}")
            lines.append("};\n")

            lines.append("#define SYSTEM_PROP_OVERRIDES_COUNT (sizeof(SYSTEM_PROP_OVERRIDES) / sizeof(SYSTEM_PROP_OVERRIDES[0]) - 1)\n")
        
        lines.append("#endif // COYOTE_CONFIG_H\n")

        content = "\n".join(lines)

        if self.dry_run:
            print(f"  [DRY-RUN] Dynamically generated {macro_count} macros.\n")
            print(content)
            print(f"\n  [DRY-RUN] Would write generated config to {output_file}")
        else:
            with open(output_file, "w") as f:
                f.write(content)
            print(f"  [+] Successfully generated {output_file} with {macro_count} dynamic macros.")

    def compile_js(self):
            """Builds the embedded JS agent: the prebuilt frida-java-bridge bundle
            (shipped by frida-tools as bridges/java.js) concatenated with agent.js,
            then emitted as a C header. No npm / Node / frida-compile — agent.js has
            no module imports; it uses the bridge via the global it exposes."""
            print("\n[+] Building JavaScript agent (frida-tools bridge + agent.js)...")
            src_js = AGENT_SRC / "js" / "agent.js"
            agent_h = BUILD_DIR / "agent.h"
            bundled_js = BUILD_DIR / "_agent.js"

            if not src_js.exists():
                print(f"[!] Error: JavaScript source file not found at {src_js}")
                sys.exit(1)

            bridge_js = _find_frida_bridge_js()
            BUILD_DIR.mkdir(parents=True, exist_ok=True)

            # java.js defines `var bridge = <Runtime>`; expose it under the name
            # agent.js reads, then append the agent. All one script.
            combined = (
                bridge_js.read_text(encoding="utf-8").rstrip()
                + "\nglobalThis.__coyote_bridge = bridge;\n"
                + src_js.read_text(encoding="utf-8")
            )

            with open(bundled_js, "w", encoding="utf-8") as f:
                f.write(combined)

            content = combined.encode("utf-8")
            hex_lines = [
                f"  {', '.join(f'0x{b:02x}' for b in content[i:i+12])},"
                for i in range(0, len(content), 12)
            ]
            header_content = (
                f"unsigned int agent_js_len = {len(content)};\n"
                f"unsigned char agent_js[] = {{\n"
                + "\n".join(hex_lines)
                + "\n};\n"
            )
            with open(agent_h, "w") as f_out:
                f_out.write(header_content)

            print(f"  [+] Bridge: {bridge_js}")
            print(f"[+] Successfully generated {agent_h} ({len(content)} bytes)")

    def _get_llvm_prebuilt_path(self) -> Path:
        """Dynamically resolves the NDK LLVM prebuilt directory matching the host OS."""
        prebuilt_dir = self.ndk_path / "toolchains" / "llvm" / "prebuilt"
        if not prebuilt_dir.exists():
            raise FileNotFoundError(f"LLVM prebuilt directory missing: {prebuilt_dir}")

        host_dirs = [d for d in prebuilt_dir.iterdir() if d.is_dir()]
        if not host_dirs:
            raise FileNotFoundError(f"No valid host toolchain directory found inside {prebuilt_dir}")

        return host_dirs[0] / "bin"

    def compile_agent(self, target_abi: str | None = None) -> Path:
        """Invokes Android NDK Clang compiler to birth puppy."""
        print("\n[+] Releasing the puppy...")

        if not self.ndk_path or not self.ndk_path.is_dir():
            print(
                f"[!] Error: Invalid NDK directory '{self.ndk_path}'. Specify via --ndk or $ANDROID_NDK_HOME."
            )
            sys.exit(1)

        # Build for the target app's ABI when known (it may run under
        # native-bridge translation, i.e. a different arch than the device's
        # primary ABI); otherwise fall back to the device's ABI.
        target_info = ABI_MAP[target_abi] if target_abi in ABI_MAP else detect_device_abi()
        compiler_prefix = target_info["compiler"]
        bin_dir = self._get_llvm_prebuilt_path()
        compiler = bin_dir / f"{compiler_prefix}{self.api_level}-clang"
        strip_tool = bin_dir / "llvm-strip"

        output_so = BUILD_DIR / "libcoyote.so"

        compile_cmd = [
            str(compiler),
            "-shared",
            "-fPIC",
            "-Wall",
            "-Os",
            "-pipe",
            # Debug info only in debug builds; it is stripped later anyway and
            # just bloats/slows the release compile.
            *(["-g3"] if self.debug else ["-g0"]),
            "-ffunction-sections",
            "-fdata-sections",
            # Debug builds keep LOGI/LOGE and the JS console; release builds
            # compile them out (quieter, fewer identifying strings in the .so).
            *(["-DCOYOTE_DEBUG"] if self.debug else []),
            # Default: run the JS/Java layer only in the main process. This flag
            # forces it into every forked process of the package.
            *(["-DCOYOTE_JS_ALL_PROCESSES"] if self.js_all_processes else []),
            # Recon mode: log intercepted probes regardless of debug/release.
            *(["-DCOYOTE_RECON"] if self.recon else []),
            f"-I{AGENT_SRC}",
            f"-I{BUILD_DIR}",
            f"-I{DEVKIT_DIR}",
            str(AGENT_SRC / "coyote.c"),
            str(AGENT_SRC / "puppyc.c"),
            str(AGENT_SRC / "puppyjs.c"),
            str(DEVKIT_DIR / "libfrida-gumjs.a"),
            "-o",
            str(output_so),
            "-Wl,--gc-sections",
            "-ldl",
            "-lm",
            "-llog",
            "-pthread",
            "-lc++_static",
            "-lc++abi",
        ]

        self._exec(compile_cmd, desc=f"NDK Shared Library Compilation ({target_info['devkit']})")

        if strip_tool.exists():
            self._exec([str(strip_tool), str(output_so)], desc="LLVM Strip Shared Library")

        print(f"[+] Puppy target: {output_so}")
        return output_so

    def launch_target_app(self, so_path: Path, nb_shim_guest_abi: str | None = None):
        """Pushes libcoyote.so into the target app's private data dir and launches
        it via the wrap property.

        Normally the wrap is `env LD_PRELOAD=<lib>`, which the OS runs through the
        device-native app_process. For a translated app (nb_shim_guest_abi set,
        the experimental --nb-shim path) that native process can't load the app's
        own libraries, so instead we drop a tiny shim that re-execs the *guest*
        (translated) app_process and hands the agent to the guest linker via
        NDK_TRANSLATION_GUEST_LD_PRELOAD — the one env channel the translator
        forwards into the guest. See docs: native-bridge injection."""

        temp_so = "/data/local/tmp/libcoyote.so"
        app_dir = f"/data/data/{self.package_name}"
        target_so = f"{app_dir}/libcoyote.so"

        self._exec(["adb", "push", str(so_path), temp_so], desc="ADB Push to Staging")

        print(f"[+] Moving agent into SELinux-compliant path: {target_so}")
        self._exec(["adb", "shell", f"su 0 cp {temp_so} {target_so}"], desc="Copy to App Dir")

        fix_perms_cmd = (
            f"su 0 sh -c '"
            f"APP_UID=$(stat -c %u {app_dir}); "
            f"chown $APP_UID:$APP_UID {target_so}; "
            f"chmod 755 {target_so}; "
            f"chcon u:object_r:app_data_file:s0 {target_so}'"
        )
        self._exec(["adb", "shell", fix_perms_cmd], desc="Apply SELinux Context & Ownership")

        print(f"[+] Injecting Puppy into target package: {self.package_name}")

        if nb_shim_guest_abi:
            self._set_native_bridge_wrap(app_dir, target_so, nb_shim_guest_abi)
        else:
            wrap_cmd = f"setprop wrap.{self.package_name} 'env LD_PRELOAD={target_so}'"
            self._exec(["adb", "shell", wrap_cmd], desc=f"Set wrap hook for {self.package_name}")

        self._exec(["adb", "shell", f"am force-stop {self.package_name}"])
        self._exec(["adb", "shell", f"monkey -p {self.package_name} -c android.intent.category.LAUNCHER 1"])

    def _set_native_bridge_wrap(self, app_dir: str, target_so: str, guest_abi: str):
        """Experimental: inject into a native-bridge-translated app. Installs a
        shim that re-execs the guest app_process with the agent preloaded via the
        translator's guest-env channel. Requires a guest app_process for the ABI
        and (today) SELinux permissive, since apps can't execute_no_trans a file
        in their own data dir under enforcing policy."""
        guest_app_process = GUEST_APP_PROCESS.get(guest_abi)
        if not guest_app_process:
            print(f"[!] No known guest app_process for ABI '{guest_abi}'; cannot use --nb-shim.")
            sys.exit(1)

        enforce = self._exec(["adb", "shell", "su 0 getenforce"]).strip()
        if enforce.lower() == "enforcing":
            print("[!] --nb-shim needs SELinux permissive (apps can't exec a shim in their")
            print("    data dir under enforcing policy). Run: adb shell su 0 setenforce 0")

        shim_path = f"{app_dir}/coyote_nb.sh"
        # The wrap prefix is invoked as `exec <wrap> <host_app_process> <args...>`,
        # so $1 is the host app_process path we drop; the rest are the real args.
        # Write locally and `adb push` it so "$@" survives (embedding it in an
        # adb-shell command line gets mangled by the intermediate shells).
        shim = (
            "#!/system/bin/sh\n"
            f"export NDK_TRANSLATION_GUEST_LD_PRELOAD={target_so}\n"
            "shift\n"
            f'exec {guest_app_process} "$@"\n'
        )
        local_shim = BUILD_DIR / "coyote_nb.sh"
        local_shim.write_text(shim)
        # Push the shim via a staging file, then place + label it. system_file is
        # the type apps are allowed to execute_no_trans (how they exec /system/bin/*).
        tmp_shim = "/data/local/tmp/coyote_nb.sh"
        self._exec(["adb", "push", str(local_shim), tmp_shim], desc="Stage NB shim")
        self._exec(["adb", "shell", f"su 0 cp {tmp_shim} {shim_path}"], desc="Copy NB shim")
        self._exec(
            ["adb", "shell",
             f"su 0 sh -c 'APP_UID=$(stat -c %u {app_dir}); chown $APP_UID:$APP_UID {shim_path}; "
             f"chmod 755 {shim_path}; chcon u:object_r:system_file:s0 {shim_path}'"],
            desc="Label NB shim",
        )
        print(f"[+] [nb-shim] guest app_process: {guest_app_process}")
        wrap_cmd = f"setprop wrap.{self.package_name} '{shim_path}'"
        self._exec(["adb", "shell", wrap_cmd], desc=f"Set native-bridge wrap for {self.package_name}")

    def reset_target_app(self):
        """Removes wrap property, stops target package, and clears app state."""
        print(f"\n[*] Resetting target package configuration: {self.package_name}")
        
        self._exec(["adb", "shell", f"am force-stop {self.package_name}"])
        self._exec(["adb", "shell", f"setprop wrap.{self.package_name} ''"])
        self._exec(["adb", "shell", f"pm clear {self.package_name}"])
        
        print(f"[+] wrap.{self.package_name} removed and process terminated.")

def capture_profile(output_path: str):
    """Captures a device profile from a connected real device via ADB and writes
    it as a Coyote profile JSON ready to edit.

    Device *facts* (build, specs, properties, GPU/CPU, network, Wi-Fi, accounts)
    are read from the device. Sections that are policy/fabricated rather than
    readable (`bypass`, `attribution`, and the synthetic parts of `identity`) are
    filled with sensible auto-generated values so the output is complete and
    usable as-is. Everything should still be reviewed before use."""
    import random

    print("\n[+] Capturing device profile over ADB...")

    def sh(cmd: str) -> str:
        try:
            out = subprocess.check_output(["adb", "shell", cmd], stderr=subprocess.DEVNULL)
            return out.decode(errors="replace").strip()
        except Exception:
            return ""

    def rand_mac() -> str:
        # Locally-administered, unicast first octet (02/06/0a/0e).
        octets = [random.choice([0x02, 0x06, 0x0A, 0x0E])] + [random.randint(0, 255) for _ in range(5)]
        return ":".join(f"{b:02x}" for b in octets)

    # --- All system properties ---
    props: dict[str, str] = {}
    for line in sh("getprop").splitlines():
        m = re.match(r"\[(.*?)\]:\s*\[(.*?)\]$", line)
        if m:
            props[m.group(1)] = m.group(2)

    if not props:
        print("[!] Could not read properties over ADB. Is a device connected and authorized?")
        sys.exit(1)

    def p(key: str, default: str = "") -> str:
        return props.get(key, default)

    generated: list[str] = []  # sections/fields we synthesized rather than read

    # --- device_profile (Build.* identity, GPU/CPU, keyboard) ---
    gl_vendor = gl_renderer = gl_version = ""
    mgl = re.search(r"GLES:\s*([^,]+),\s*([^,]+),\s*(.+)", sh("dumpsys SurfaceFlinger | grep -i GLES"))
    if mgl:
        gl_vendor, gl_renderer, gl_version = (g.strip() for g in mgl.groups())
    cpu_info = sh("cat /proc/cpuinfo")
    keyboard = sh("settings get secure default_input_method")

    device_profile = {
        "profile_id": f"{p('ro.product.brand')}-{p('ro.product.model')}".lower().replace(" ", "-").strip("-"),
        "marketing_name": p("ro.product.model"),
        "model": p("ro.product.model"),
        "device": p("ro.product.device"),
        "board": p("ro.product.board"),
        "product": p("ro.product.name"),
        "hardware": p("ro.hardware"),
        "fingerprint": p("ro.build.fingerprint"),
        "manufacturer": p("ro.product.manufacturer"),
        "bootloader": p("ro.bootloader"),
        "brand": p("ro.product.brand"),
        "host": p("ro.build.host"),
        "display": p("ro.build.display.id"),
        "display_id": p("ro.build.id"),
        "tags": p("ro.build.tags"),
        "serial": p("ro.serialno"),
        "type": p("ro.build.type"),
        "user": p("ro.build.user"),
        "arch": p("ro.product.cpu.abi"),
        "baseband": p("gsm.version.baseband"),
        "characteristics": p("ro.build.characteristics"),
        "flavor": p("ro.build.flavor"),
        "incremental": p("ro.build.version.incremental"),
        "build_date": p("ro.build.date"),
        "build_date_utc": p("ro.build.date.utc"),
        "security_patch": p("ro.build.version.security_patch"),
        "soc_model": p("ro.soc.model"),
        "gl_vendor": gl_vendor,
        "gl_renderer": gl_renderer,
        "gl_version": gl_version,
        "keyboard": keyboard if keyboard and keyboard != "null" else "",
        "cpu_info": cpu_info,
    }
    device_profile = {k: v for k, v in device_profile.items() if v}

    # --- specs (display / memory / storage / battery / swap) ---
    specs: dict = {}
    m = re.search(r"Physical size:\s*(\d+)x(\d+)", sh("wm size"))
    if m:
        specs["WIDTH"], specs["HEIGHT"] = int(m.group(1)), int(m.group(2))
    m = re.search(r"Physical density:\s*(\d+)", sh("wm density"))
    if m:
        dpi = int(m.group(1))
        specs["DENSITY_DPI"] = dpi
        specs["DENSITY"] = f"{dpi / 160.0}f"
        specs["XDPI"] = specs["YDPI"] = f"{float(dpi)}f"
    for line in sh("cat /proc/meminfo").splitlines():
        mm = re.match(r"(MemTotal|MemAvailable|SwapTotal):\s*(\d+)\s*kB", line)
        if mm:
            if mm.group(1) == "SwapTotal":
                specs["SWAP_TOTAL_KB"] = int(mm.group(2))
            else:
                specs["TOTAL_RAM" if mm.group(1) == "MemTotal" else "AVAIL_RAM"] = int(mm.group(2)) * 1024
    df = sh("df /data")
    md = re.search(r"\s(\d+)\s+(\d+)\s+(\d+)\s+\d+%", df)
    if md:  # columns: 1K-blocks used available
        specs["TOTAL_SPACE"] = int(md.group(1)) * 1024
        specs["FREE_SPACE"] = int(md.group(3)) * 1024
    mb = re.search(r"level:\s*(\d+)", sh("dumpsys battery"))
    if mb:
        specs["BATTERY_LEVEL"] = int(mb.group(1))
    if "TOTAL_RAM" in specs:  # derived (not directly readable): ~5% low-memory threshold
        specs["RAM_THRESHOLD"] = int(specs["TOTAL_RAM"] * 0.05)
        generated.append("specs.RAM_THRESHOLD (derived)")

    # --- telecom (gsm.* props + locale/timezone; auto-fill the rest) ---
    SIM_STATES = {"ABSENT": 1, "PIN_REQUIRED": 2, "PUK_REQUIRED": 3, "NETWORK_LOCKED": 4, "READY": 5}
    telecom: dict = {}
    numeric = p("gsm.operator.numeric")
    if len(numeric) >= 5:
        telecom["mcc"], telecom["mnc"] = numeric[:3], numeric[3:]
    if p("gsm.operator.alpha"):
        telecom["operator_name"] = p("gsm.operator.alpha")
    iso = p("gsm.operator.iso-country")
    if iso:
        telecom["country_iso"] = iso
    locale = p("persist.sys.locale") or p("ro.product.locale")
    if "-" in locale:
        telecom["language"] = locale.split("-")[0]
    if p("persist.sys.timezone"):
        telecom["timezone_id"] = p("persist.sys.timezone")
    sim_state = p("gsm.sim.state").split(",")[0].strip().upper()
    if sim_state in SIM_STATES:
        telecom["sim_state"] = sim_state
        telecom["sim_state_num"] = SIM_STATES[sim_state]
    # Not device-readable without privileged APIs, synthesize plausible values.
    if telecom:
        telecom.setdefault("network_type", "LTE")
        telecom.setdefault("imei_prefix", "35")
        generated.append("telecom.network_type/imei_prefix (synthesized)")

    # --- network (local IPs + active transport heuristic) ---
    network: dict = {}
    mobile_ip = wifi_ip = ""
    for line in sh("ip -o -4 addr show").splitlines():
        mi = re.search(r"\d+:\s+(\S+)\s+inet\s+(\d+\.\d+\.\d+\.\d+)", line)
        if not mi:
            continue
        iface, ip = mi.group(1), mi.group(2)
        if iface.startswith("wlan"):
            wifi_ip = ip
        elif iface.startswith("rmnet") or iface.startswith("ccmni"):
            mobile_ip = ip
    if wifi_ip:
        network["wifi_local_ip"] = wifi_ip
    if mobile_ip:
        network["mobile_data_local_ip"] = mobile_ip
    network["net_type"] = 1 if wifi_ip else 0  # 1 = Wi-Fi, 0 = mobile

    # --- identity (capture Wi-Fi/account; auto-generate what can't be read) ---
    identity: dict = {}
    mssid = re.search(r'SSID:\s*"?([^",\n]+)"?', sh("cmd wifi status"))
    if mssid and mssid.group(1).strip() not in ("", "<unknown ssid>", "<none>"):
        identity["wifi_ssid"] = mssid.group(1).strip()
    else:
        identity["wifi_ssid"] = random.choice(["HomeWiFi", "NETGEAR", "TP-Link_", "Linksys"]) + str(random.randint(10, 99))
        generated.append("identity.wifi_ssid (synthesized)")
    # BSSID/MAC are randomized/redacted on modern Android, synthesize.
    identity["wifi_bssid"] = rand_mac()
    identity["wifi_mac"] = rand_mac()
    macc = re.search(r"name=([^\s,}]+@[^\s,}]+)", sh("dumpsys account"))
    if macc:
        identity["account_email"] = macc.group(1)
    else:
        identity["account_email"] = f"{random.choice(['alex','sam','jordan','taylor','casey'])}.{random.choice(['lee','khan','silva','jones'])}{random.randint(1, 99)}@gmail.com"
        generated.append("identity.account_email (synthesized)")
    identity["account_type"] = "com.google"
    generated.append("identity.wifi_bssid/wifi_mac (synthesized)")

    # --- location: last-known GPS fix from the device (if any) ---
    location: dict = {}
    mloc = re.search(r"Location\[\w+\s+([-\d.]+),\s*([-\d.]+)", sh("dumpsys location"))
    if mloc:
        location["latitude"] = float(mloc.group(1))
        location["longitude"] = float(mloc.group(2))
        macc = re.search(r"hAcc=([\d.]+)", sh("dumpsys location"))
        location["accuracy"] = float(macc.group(1)) if macc else 20.0
    # else left empty, the build derives a fix from telecom.country_iso.

    # --- bypass / attribution: policy & fabricated, auto-generated defaults ---
    bypass = {"flag_secure": 1}
    attribution = {
        "facebook_app_id": "",
        "af_status": "Organic",
        "campaign": "",
        "install_referrer": "utm_source=google-play&utm_medium=organic",
        "install_package": "com.android.vending",
    }
    generated.append("bypass + attribution (auto-generated defaults)")

    # --- system_prop: the detection-relevant keys present on the device ---
    PROP_PREFIXES = (
        "ro.product.", "ro.build.", "ro.boot.", "ro.vendor.", "ro.odm.",
        "ro.system.", "ro.system_ext.", "ro.boottime.", "init.svc.", "init.svc_debug_pid.",
        "ro.lineage", "ro.product.vendor", "gsm.",
    )
    PROP_EXACT = {
        "ro.hardware", "ro.board.platform", "ro.soc.model", "ro.serialno",
        "ro.bootloader", "ro.debuggable", "ro.secure", "ro.adb.secure",
        "ro.kernel.qemu", "ro.warranty_bit", "ro.crypto.state", "ro.sf.lcd_density",
        "ro.modversion", "sys.oem_unlock_allowed", "vendor.sys.slpi.firmware.version",
        "persist.sys.timezone", "persist.sys.locale", "persist.sys.language",
        "persist.sys.usb.config", "sys.usb.config", "sys.usb.state", "sys.usb.ffs.ready",
        "service.adb.root", "service.adb.tcp.port", "http.proxyHost",
        "qemu.sf.lcd_density",
    }

    def wanted(k: str) -> bool:
        if k in PROP_EXACT:
            return True
        if k.startswith(PROP_PREFIXES):
            # skip obviously non-detection noise
            return not k.endswith(".enable_iorap") and "fingerprintd" not in k
        return k.startswith("qemu.") or "verifiedboot" in k or "flash.locked" in k

    system_prop = {k: v for k, v in sorted(props.items()) if wanted(k)}

    profile = {
        "telecom": telecom,
        "bypass": bypass,
        "device_profile": device_profile,
        "specs": specs,
        "network": network,
        "attribution": attribution,
        "identity": identity,
        "location": location,
        "system_prop": system_prop,
    }
    profile = {k: v for k, v in profile.items() if v}

    with open(output_path, "w") as f:
        json.dump(profile, f, indent=2)

    print(f"[+] Wrote captured profile to {output_path}")
    print(f"    Captured {len(system_prop)} system properties and "
          f"{len(device_profile)} device fields.")
    if generated:
        print("    Auto-generated (review these, not read from the device):")
        for g in generated:
            print(f"      - {g}")


def _installed_frida_version() -> str | None:
    """Version of the installed `frida` Python package (ships with frida-tools)."""
    try:
        import frida
        return frida.__version__
    except Exception:
        return None


def _find_frida_bridge_js() -> Path:
    """Locate frida-tools' prebuilt frida-java-bridge bundle (bridges/java.js)."""
    try:
        import frida_tools
    except ImportError:
        print("[!] frida-tools is not installed. Install it first, e.g.:")
        print("      python3 -m venv .venv && . .venv/bin/activate && pip install frida-tools")
        sys.exit(1)
    bridge = Path(frida_tools.__file__).resolve().parent / "bridges" / "java.js"
    if not bridge.is_file():
        print(f"[!] frida-java-bridge bundle not found at {bridge}")
        print("    (expected it in frida-tools; try upgrading frida-tools)")
        sys.exit(1)
    return bridge


def update_frida_devkit(arch: str | None = None):
    """Fetches the Frida-GumJS DevKit matching the installed frida-tools version.

    Pinning to the same Frida version keeps the embedded GumJS runtime (devkit)
    in lockstep with the bridge JS from frida-tools, so the bridge never calls a
    runtime API the devkit doesn't provide. Arch is auto-detected via ADB if
    omitted."""

    if not arch or arch == "auto":
        device_info = detect_device_abi()
        arch = device_info["devkit"]
        print(f"[+] Auto-detected devkit architecture via ADB: {arch}")

    version = _installed_frida_version()
    if not version:
        print("[!] frida not installed; cannot pin the devkit version. Install frida-tools.")
        sys.exit(1)

    platform_target = f"android-{arch}" if not arch.startswith("android-") else arch
    asset = f"frida-gumjs-devkit-{version}-{platform_target}.tar.xz"
    archive_url = f"https://github.com/frida/frida/releases/download/{version}/{asset}"
    print(f"[+] Fetching devkit for frida {version} ({platform_target})...")

    archive_path = DEVKIT_DIR / "devkit.tar.xz"
    DEVKIT_DIR.mkdir(parents=True, exist_ok=True)
    try:
        req = urllib.request.Request(archive_url, headers={"User-Agent": "Coyote-Provisioner"})
        with urllib.request.urlopen(req) as resp, open(archive_path, "wb") as out:
            out.write(resp.read())
    except Exception as e:
        print(f"[!] Failed to download {asset}: {e}")
        print(f"    URL: {archive_url}")
        sys.exit(1)

    print(f"[+] Extracting headers and static libraries into {DEVKIT_DIR}...")
    with tarfile.open(archive_path, "r:xz") as tar:
        if hasattr(tarfile, "data_filter"):
            tar.extractall(path=DEVKIT_DIR, filter="data")
        else:
            tar.extractall(path=DEVKIT_DIR)

    archive_path.unlink()
    # Record which arch is cached so ensure_devkit() can refetch when the build
    # target changes (e.g. switching from an x86_64 device to an arm64 app).
    try:
        (DEVKIT_DIR / ".devkit_arch").write_text(arch)
    except Exception:
        pass
    print(f"[+] Frida-GumJS DevKit {version} ({platform_target}) installed.")

def ensure_devkit(requested_arch: str | None = None):
    """Verifies DevKit files exist. Downloads them if missing or explicitly requested."""

    header_file = DEVKIT_DIR / "frida-gumjs.h"
    lib_file = DEVKIT_DIR / "libfrida-gumjs.a"

    arch_marker = DEVKIT_DIR / ".devkit_arch"
    cached_arch = arch_marker.read_text().strip() if arch_marker.is_file() else None

    if requested_arch and requested_arch not in ("auto", None):
        # Already have exactly this arch? Don't re-download.
        if header_file.is_file() and lib_file.is_file() and cached_arch == requested_arch:
            return
        update_frida_devkit(requested_arch)
        return

    if header_file.is_file() and lib_file.is_file():
        return

    print(f"\n[!] Frida-GumJS DevKit missing from {DEVKIT_DIR}")

    print("[+] Attempting to auto-detect target device ABI via ADB...")
    try:
        device_info = detect_device_abi()
        chosen_arch = device_info["devkit"]
        print(f"[+] Auto-detected architecture from device: {chosen_arch}")
        update_frida_devkit(chosen_arch)
        return
    except Exception as e:
        print(f"[!] Could not auto-detect via ADB ({e}).")

    if sys.stdin.isatty():
        valid_archs = ["arm", "arm64", "x86", "x86_64"]
        print("    First-time setup detected. DevKit binaries must be downloaded.")
        selection = input("    Select architecture [arm | arm64 | x86 | x86_64] (default: arm64): ").strip().lower()

        chosen_arch = selection if selection in valid_archs else "arm64"
        print(f"    Selected target: {chosen_arch}")
        update_frida_devkit(chosen_arch)
    else:
        print("[!] Non-interactive shell detected. Run with --update-devkit <arch> to initialize.")
        sys.exit(1)

def parse_args():
    parser = argparse.ArgumentParser(
        prog="coyote",
        description="Coyote - Autonomous Anti-Analysis Environment Provisioner",
        formatter_class=argparse.ArgumentDefaultsHelpFormatter,
    )

    parser.add_argument(
        "-p",
        "--profile",
        help="Path to JSON profile configuration file.",
    )
    parser.add_argument(
        "--ndk",
        default=os.getenv("ANDROID_NDK_HOME"),
        help="Path to Android NDK directory (defaults to $ANDROID_NDK_HOME).",
    )
    parser.add_argument(
        "-t",
        "--target",
        help="Target app package name",
    )
    parser.add_argument(
        "-s",
        "--serial",
        help="ADB device serial to target (from `adb devices`). Scopes every ADB "
        "call to that device, useful when a physical phone and an emulator are "
        "both connected (e.g. --capture from the phone, deploy to the emulator).",
    )
    parser.add_argument(
        "--reset-target",
        action="store_true",
        help="Reset target app",
    )
    parser.add_argument(
        "--api-level",
        type=int,
        default=30,
        help="Target Android API level for NDK compilation.",
    )
    parser.add_argument(
        "--dry-run",
        action="store_true",
        help="Print actions and commands without modifying system or compiling.",
    )
    parser.add_argument(
        "--debug",
        action="store_true",
        help="Build with logging enabled (native LOGI/LOGE + JS console). "
        "Default (release) builds are silent and carry fewer identifying strings.",
    )
    parser.add_argument(
        "--js-all-processes",
        action="store_true",
        help="Run the JS/Java layer in every forked process of the package. "
        "Default runs it only in the main UI process (native hooks still apply "
        "everywhere). Use this if a target runs detection in a sub-process.",
    )
    parser.add_argument(
        "--recon",
        action="store_true",
        help="Build in recon mode: log every intercepted probe (property reads, "
        "getprop, Build fields, key detection APIs) under the [CoyoteRecon] tag, "
        "so you can see what the target checks. Independent of --debug.",
    )
    parser.add_argument(
        "--capture",
        metavar="OUT.json",
        help="Capture a device profile from a connected real device via ADB, "
        "write it to OUT.json, then exit. Review/edit before use.",
    )
    parser.add_argument(
        "--skip-adb",
        action="store_true",
        help="Skip executing setprop commands over ADB.",
    )
    parser.add_argument(
        "--skip-deploy",
        action="store_true",
        help="Compile binary but skip ADB push to target device.",
    )
    parser.add_argument(
        "--update-devkit",
        choices=["auto", "arm", "arm64", "x86", "x86_64"],
        nargs="?",
        const="auto",
        help="Fetch latest Frida-GumJS DevKit for specified architecture (auto-detects via ADB if omitted).",
    )
    parser.add_argument(
        "--nb-shim",
        action="store_true",
        help="EXPERIMENTAL: instrument a native-bridge-translated app (e.g. an "
        "arm64-only app on an x86_64 emulator) by re-execing the guest "
        "app_process and preloading the agent via the translator's guest-env "
        "channel. Requires SELinux permissive. Known unstable (frida-gum under "
        "translation may crash); without it, a translated target is refused.",
    )

    return parser.parse_args()

def main():
    args = parse_args()

    if args.skip_deploy:
        args.skip_adb = True

    # Scope every ADB call (capture, ABI detection, devkit, deploy, reset) to a
    # specific device. adb honors ANDROID_SERIAL, and all subprocesses inherit
    # it, so this covers every `adb` invocation without threading a parameter
    # through each call site.
    if args.serial:
        os.environ["ANDROID_SERIAL"] = args.serial

    print("════════════════════════════════════════════════════")
    print("                       COYOTE                       ")
    print("════════════════════════════════════════════════════")
    if args.dry_run:
        print("[!] DRY-RUN MODE ACTIVE: No changes will be executed.")

    if args.update_devkit is not None:
        ensure_devkit(args.update_devkit)
        return

    if args.capture:
        capture_profile(args.capture)
        return

    orchestrator = Coyote(
        profile_path=args.profile,
        ndk_path=args.ndk,
        api_level=args.api_level,
        dry_run=args.dry_run,
        package_name=args.target,
        debug=args.debug,
        js_all_processes=args.js_all_processes,
        recon=args.recon,
    )

    if args.reset_target:
        orchestrator.reset_target_app()
        if not args.profile:
            return

    if not args.skip_adb and args.profile:
        orchestrator.configure_adb_properties()

    if args.profile:
        orchestrator.validate_profile()
        orchestrator.generate_c_header()

        # Decide which ABI to build for and whether the target only runs under
        # native-bridge translation. Skip when we can't query the device (dry
        # run / no ADB / no target) — then the build falls back to device ABI.
        target_abi = None
        nb_shim_guest_abi = None
        if args.target and not args.dry_run and not args.skip_adb:
            info = resolve_target_abi(args.target)
            target_abi = info["build_abi"]
            if info["app_abi"]:
                print(f"\n[+] Target ABI: app={info['app_abi']} device={info['device_abi']}"
                      + (" (translated via native bridge)" if info["translated"] else ""))
            if info["translated"]:
                if args.nb_shim:
                    print("[!] --nb-shim: using EXPERIMENTAL native-bridge injection (unstable).")
                    nb_shim_guest_abi = info["app_abi"]
                else:
                    print(
                        f"\n[!] '{args.target}' runs as {info['app_abi']} but this device is "
                        f"{info['device_abi']} — it only runs through native-bridge "
                        "translation, which Coyote's wrap+LD_PRELOAD injection cannot cross.\n"
                        "    The app would hang on its splash (its own native libs fail to "
                        "load in the forced non-native process).\n"
                        "    Fix: run this app on a matching-ABI target (an arm64 system image "
                        "or a real arm64 device), where injection works natively.\n"
                        "    Or try the experimental --nb-shim path (requires SELinux permissive; "
                        "currently unstable)."
                    )
                    sys.exit(2)

        ensure_devkit(ABI_MAP[target_abi]["devkit"] if target_abi in ABI_MAP else None)
        orchestrator.compile_js()
        agent_so = orchestrator.compile_agent(target_abi=target_abi)

        if not args.skip_deploy:
            orchestrator.launch_target_app(agent_so, nb_shim_guest_abi=nb_shim_guest_abi)

if __name__ == "__main__":
    main()