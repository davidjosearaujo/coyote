#!/usr/bin/env python3
import argparse
import json
import tarfile
import os
import re
import subprocess
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
    """Queries target device via ADB to dynamically resolve CPU ABI and compiler triple."""
    try:
        cmd = ["adb", "shell", "getprop", "ro.product.cpu.abi"]
        raw_abi = subprocess.check_output(cmd).decode().strip()
        if raw_abi in ABI_MAP:
            return ABI_MAP[raw_abi]
        print(f"[!] Unknown ABI '{raw_abi}'. Defaulting to arm64-v8a.")
    except Exception as e:
        print(f"[!] ADB query failed ({e}). Defaulting compiler to arm64-v8a.")
    
    return ABI_MAP["arm64-v8a"]

class Coyote:

    def __init__(
        self,
        profile_path: str | None = None,
        ndk_path: str | None = None,
        api_level: int = 30,
        dry_run: bool = False,
        package_name: str = "test.target.app",
    ):
        self.dry_run = dry_run
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
            ("2. Device Identification & Build Metadata", "device_profile", "DEVICE_"),
            ("3. Hardware Metrics & Memory Space", "specs", "SPECS_"),
            ("4. Network Configuration", "network", "NETWORK_"),
            ("5. Attribution & Install Tracking", "attribution", "ATTRIBUTION_")
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
            for raw_key, macro_name in prop_overrides:
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

        output_so = BUILD_DIR / "libpuppy.so"

        compile_cmd = [
            str(compiler),
            "-shared",
            "-fPIC",
            "-Wall",
            "-Os",
            "-pipe",
            "-g3",
            "-ffunction-sections",
            "-fdata-sections",
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
        """Pushes libpuppy.so and libc++_shared.so into target app private data directory and launches package via wrap property."""

        target_info = detect_device_abi()

        temp_so = "/data/local/tmp/libpuppy.so"
        app_dir = f"/data/data/{self.package_name}"
        target_so = f"{app_dir}/libpuppy.so"

        self._exec(["adb", "push", str(so_path), temp_so], desc="ADB Push to Staging")

        print(f"[+] Moving agent into SELinux-compliant path: {target_so}")
        self._exec(["adb", "shell", f"su 0 cp {temp_so} {target_so}"], desc="Copy to App Dir")

        # Fix Ownership & SELinux Security Context
        # Resolves app UID/GID dynamically and applies app_data_file SELinux label
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

    print("════════════════════════════════════════════════════")
    print("                       COYOTE                       ")
    print("════════════════════════════════════════════════════")
    if args.dry_run:
        print("[!] DRY-RUN MODE ACTIVE: No changes will be executed.")

    if args.update_devkit is not None:
        ensure_devkit(args.update_devkit)
        return

    orchestrator = Coyote(
        profile_path=args.profile,
        ndk_path=args.ndk,
        api_level=args.api_level,
        dry_run=args.dry_run,
        package_name=args.target,
    )

    if args.reset_target:
        orchestrator.reset_target_app()
        if not args.profile:
            return

    if not args.skip_adb and args.profile:
        orchestrator.configure_adb_properties()

    if args.profile:
        orchestrator.generate_c_header()
        ensure_devkit()
        agent_so = orchestrator.compile_agent()

        if not args.skip_deploy:
            orchestrator.launch_target_app(agent_so)

if __name__ == "__main__":
    main()