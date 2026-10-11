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

BASE_DIR = Path(__file__).parent.resolve()
DEVKIT_DIR = Path(__file__).parent / "agent_src" / "devkit"
AGENT_SRC = BASE_DIR / "agent_src"
BUILD_DIR = BASE_DIR / "build"

ABI_MAP = {
    "arm64-v8a": {"compiler": "aarch64-linux-android", "devkit": "arm64"},
    "armeabi-v7a": {"compiler": "armv7a-linux-androideabi", "devkit": "arm"},
    "x86_64": {"compiler": "x86_64-linux-android", "devkit": "x86_64"},
    "x86": {"compiler": "i686-linux-android", "devkit": "x86"},
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
            BUILD_DIR.mkdir(exist_ok=True)

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
            "network", "attribution", "identity", "system_prop",
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
                        f"('{sysp['gsm.operator.numeric']}') — a detector comparing them would notice."
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

    def generate_c_header(self):
        """Dynamically generates config.h from the JSON profile without a template."""
        print("\n[+] Generating agent_src/config.h header file...")
        output_file = BUILD_DIR / "config.h"

        def format_value(key: str, val) -> str:
            """Infers the correct C-type formatting for the macro value."""
            if key == "CPU_INFO":
                return f'R"raw_cpu({val})raw_cpu"'
            if isinstance(val, str):
                return f'"{val}"'
            if isinstance(val, float):
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
            """Bundles agent.js using frida-compile and converts it to a C header."""
            print("\n[+] Compiling JavaScript agent with frida-compile...")
            src_js = AGENT_SRC / "js" / "agent.js"
            bundled_js = BUILD_DIR / "_agent.js"
            agent_h = BUILD_DIR / "agent.h"

            if not src_js.exists():
                print(f"[!] Error: JavaScript source file not found at {src_js}")
                sys.exit(1)

            BUILD_DIR.mkdir(exist_ok=True)

            build_cmd = ["frida-compile", "-c", "--no-source-maps", str(src_js), "-o", str(bundled_js),]
            result = subprocess.run(build_cmd, cwd=str(BASE_DIR), capture_output=True, text=True)
            if result.returncode != 0:
                print(f"[!] frida-compile failed:\n{result.stderr.strip()}")
                sys.exit(1)
            print(f"  [+] frida-compile succeeded")

            with open(bundled_js, "r", encoding="utf-8") as f_in:
                js_content = f_in.read()

            if "✄" in js_content:
                js_content = js_content.split("✄")[-1].lstrip()

            content = js_content.encode("utf-8")

            hex_lines = [
                f"  {', '.join(f'0x{b:02x}' for b in content[i:i+12])},"
                for i in range(0, len(content), 12)
            ]

            header_content = (
                f"unsigned int agent_js_len = {len(content)};\n"
                f"unsigned char agent_js[] = {{\n"
                + "\n".join(hex_lines)
                + f"\n}};\n"
            )

            with open(agent_h, "w") as f_out:
                f_out.write(header_content)

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

    def compile_agent(self) -> Path:
        """Invokes Android NDK Clang compiler to birth puppy."""
        print("\n[+] Releasing the puppy...")

        if not self.ndk_path or not self.ndk_path.is_dir():
            print(
                f"[!] Error: Invalid NDK directory '{self.ndk_path}'. Specify via --ndk or $ANDROID_NDK_HOME."
            )
            sys.exit(1)

        target_info = detect_device_abi()
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
            f"-I{AGENT_SRC / 'devkit'}",
            str(AGENT_SRC / "coyote.c"),
            str(AGENT_SRC / "puppyc.c"),
            str(AGENT_SRC / "puppyjs.c"),
            str(AGENT_SRC / "devkit" / "libfrida-gumjs.a"),
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

    def launch_target_app(self, so_path: Path):
        """Pushes libcoyote.so and libc++_shared.so into target app private data directory and launches package via wrap property."""

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

        wrap_cmd = f"setprop wrap.{self.package_name} 'env LD_PRELOAD={target_so}'"
        self._exec(["adb", "shell", wrap_cmd], desc=f"Set wrap hook for {self.package_name}")

        self._exec(["adb", "shell", f"am force-stop {self.package_name}"])
        self._exec(["adb", "shell", f"monkey -p {self.package_name} -c android.intent.category.LAUNCHER 1"])

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
    # Not device-readable without privileged APIs — synthesize plausible values.
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
    # BSSID/MAC are randomized/redacted on modern Android — synthesize.
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

    # --- bypass / attribution: policy & fabricated — auto-generated defaults ---
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
        "system_prop": system_prop,
    }
    profile = {k: v for k, v in profile.items() if v}

    with open(output_path, "w") as f:
        json.dump(profile, f, indent=2)

    print(f"[+] Wrote captured profile to {output_path}")
    print(f"    Captured {len(system_prop)} system properties and "
          f"{len(device_profile)} device fields.")
    if generated:
        print("    Auto-generated (review these — not read from the device):")
        for g in generated:
            print(f"      - {g}")


def update_frida_devkit(arch: str | None = None):
    """Fetches and extracts the latest Frida-GumJS DevKit for specified architecture (auto-detects via ADB if omitted)."""

    if not arch or arch == "auto":
        device_info = detect_device_abi()
        arch = device_info["devkit"]
        print(f"[+] Auto-detected devkit architecture via ADB: {arch}")

    platform_target = f"android-{arch}" if not arch.startswith("android-") else arch
    print(f"[+] Checking latest Frida release for platform: {platform_target}...")
    
    api_url = "https://api.github.com/repos/frida/frida/releases/latest"
    req = urllib.request.Request(api_url, headers={"User-Agent": "Coyote-Provisioner"})
    
    with urllib.request.urlopen(req) as resp:
        release_data = json.loads(resp.read().decode())
    
    tag_name = release_data["tag_name"]
    print(f"[+] Latest Frida version identified: {tag_name}")
    
    target_asset = None
    prefix = f"frida-gumjs-devkit-{tag_name}-{platform_target}.tar.xz"
    
    for asset in release_data.get("assets", []):
        if asset["name"] == prefix:
            target_asset = asset
            break
            
    if not target_asset:
        raise RuntimeError(f"Could not find matching release asset: {prefix}")
        
    archive_url = target_asset["browser_download_url"]
    archive_path = DEVKIT_DIR / "devkit_latest.tar.xz"
    DEVKIT_DIR.mkdir(parents=True, exist_ok=True)
    
    print(f"[+] Downloading: {target_asset['name']}...")
    urllib.request.urlretrieve(archive_url, archive_path)
    
    print("[+] Extracting headers and static libraries into agent_src/devkit...")
    with tarfile.open(archive_path, "r:xz") as tar:
        if hasattr(tarfile, "data_filter"):
            tar.extractall(path=DEVKIT_DIR, filter="data")
        else:
            tar.extractall(path=DEVKIT_DIR)
        
    archive_path.unlink()
    print("[+] Frida-GumJS DevKit updated successfully.")

def ensure_devkit(requested_arch: str | None = None):
    """Verifies DevKit files exist. Downloads them if missing or explicitly requested."""

    header_file = DEVKIT_DIR / "frida-gumjs.h"
    lib_file = DEVKIT_DIR / "libfrida-gumjs.a"

    if requested_arch:
        update_frida_devkit(requested_arch)
        return

    if header_file.is_file() and lib_file.is_file():
        return

    print("\n[!] Frida-GumJS DevKit missing from agent_src/devkit/")

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
        "call to that device — useful when a physical phone and an emulator are "
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
        ensure_devkit()
        orchestrator.compile_js()
        agent_so = orchestrator.compile_agent()

        if not args.skip_deploy:
            orchestrator.launch_target_app(agent_so)

if __name__ == "__main__":
    main()