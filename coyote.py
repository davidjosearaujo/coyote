#!/usr/bin/env python3
import argparse
import json
import os
import re
import subprocess
import sys
from pathlib import Path

BASE_DIR = Path(__file__).parent.resolve()
AGENT_SRC = BASE_DIR / "agent_src"
BUILD_DIR = BASE_DIR / "build"


class Coyote:

    def __init__(
        self,
        profile_path: str,
        ndk_path: str,
        api_level: int = 30,
        dry_run: bool = False,
    ):
        self.dry_run = dry_run
        self.api_level = api_level
        self.ndk_path = Path(ndk_path) if ndk_path else None
        self.profile = self._load_profile(profile_path)

        if not dry_run:
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
            print(f"    - setprop {prop} -> {val}")
            self._exec(
                ["adb", "shell", "setprop", prop, str(val)],
                desc=f"setprop {prop}",
            )

    def _sanitize_macro_key(self, prop_key: str) -> str:
        """Converts property keys like 'ro.build.display.id' to 'SYS_PROP_RO_BUILD_DISPLAY_ID'."""
        sanitized = re.sub(r"[^a-zA-Z0-9_]", "_", prop_key).upper()
        return f"SYS_PROP_{sanitized}"

    def generate_c_header(self):
        """Injects all nested profile values into config.h.template."""
        print("\n[+] Generating agent_src/config.h header file...")
        template_file = AGENT_SRC / "config.h.template"
        output_file = AGENT_SRC / "config.h"

        if not template_file.is_file():
            print(f"[!] Error: Template '{template_file}' missing.")
            sys.exit(1)

        with open(template_file, "r") as f:
            content = f.read()

        replacements = {}

        # 1. Telecom section
        telecom = self.profile.get("telecom", {})
        for k, v in telecom.items():
            replacements[f"{{{{TELECOM_{k.upper()}}}}}"] = str(v)

        # 2. Device profile section
        dev = self.profile.get("device_profile", {})
        for k, v in dev.items():
            replacements[f"{{{{DEVICE_{k.upper()}}}}}"] = str(v)

        # 3. Hardware specs section
        specs = self.profile.get("specs", {})
        for k, v in specs.items():
            replacements[f"{{{{SPECS_{k.upper()}}}}}"] = str(v)

        # 4. Network section
        net = self.profile.get("network", {})
        for k, v in net.items():
            replacements[f"{{{{NETWORK_{k.upper()}}}}}"] = str(v)

        # 5. Attribution section
        attr = self.profile.get("attribution", {})
        for k, v in attr.items():
            replacements[f"{{{{ATTRIBUTION_{k.upper()}}}}}"] = str(v)

        # 6. System Properties section
        sys_props = self.profile.get("system_prop", {})
        for raw_key, val in sys_props.items():
            macro_placeholder = f"{{{{{self._sanitize_macro_key(raw_key)}}}}}"
            replacements[macro_placeholder] = str(val)

        # Perform placeholder substitutions
        for placeholder, value in replacements.items():
            content = content.replace(placeholder, value)

        if self.dry_run:
            print(
                f"  [DRY-RUN] Replaced {len(replacements)} placeholders in template.\n"
            )
            print(content)
            print(f"\n  [DRY-RUN] Would write generated config to {output_file}")
        else:
            with open(output_file, "w") as f:
                f.write(content)
            print(
                f"  [+] Successfully injected {len(replacements)} profile parameters into {output_file}"
            )

    def compile_agent(self) -> Path:
        """Invokes Android NDK Clang compiler to produce stealth_agent binary."""
        print("\n[+] Compiling autonomous stealth agent...")

        if not self.ndk_path or not self.ndk_path.is_dir():
            print(
                f"[!] Error: Invalid NDK directory '{self.ndk_path}'. Specify via --ndk or $ANDROID_NDK_HOME."
            )
            sys.exit(1)

        compiler = (
            self.ndk_path
            / "toolchains/llvm/prebuilt/linux-x86_64/bin"
            / f"aarch64-linux-android{self.api_level}-clang"
        )
        strip_tool = (
            self.ndk_path
            / "toolchains/llvm/prebuilt/linux-x86_64/bin/llvm-strip"
        )

        output_binary = BUILD_DIR / "stealth_agent"

        compile_cmd = [
            str(compiler),
            "-O3",
            "-s",
            f"-I{AGENT_SRC}",
            f"-I{AGENT_SRC / 'devkit'}",
            str(AGENT_SRC / "agent_main.c"),
            str(AGENT_SRC / "devkit" / "libfrida-gum.a"),
            "-o",
            str(output_binary),
            "-ldl",
            "-lm",
            "-llog",
            "-pthread",
        ]

        self._exec(compile_cmd, desc="NDK Compilation")

        strip_cmd = [str(strip_tool), "--strip-all", str(output_binary)]
        self._exec(strip_cmd, desc="Symbol Stripping")

        print(f"[+] Agent binary target: {output_binary}")
        return output_binary

    def deploy_and_execute(self, binary_path: Path):
        """Pushes compiled agent to target device via ADB."""
        remote_path = "/data/local/tmp/stealth_agent"
        print(f"\n[+] Deploying agent to target ({remote_path})...")

        self._exec(
            ["adb", "push", str(binary_path), remote_path], desc="ADB Push"
        )
        self._exec(["adb", "shell", "chmod", "755", remote_path], desc="Chmod Agent")

        print("[+] Deployment complete.")


def parse_args():
    parser = argparse.ArgumentParser(
        description="Coyote - Autonomous Anti-Analysis Environment Provisioner",
        formatter_class=argparse.ArgumentDefaultsHelpFormatter,
    )

    parser.add_argument(
        "-p",
        "--profile",
        required=True,
        help="Path to JSON profile configuration file.",
    )
    parser.add_argument(
        "--ndk",
        default=os.getenv("ANDROID_NDK_HOME"),
        help="Path to Android NDK directory (defaults to $ANDROID_NDK_HOME).",
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

    return parser.parse_args()


def main():
    args = parse_args()

    print("═══════════════════════════════════════════════════")
    print("  COYOTE: Anti-Evasion Agent & Profile Pipeline    ")
    print("═══════════════════════════════════════════════════")
    if args.dry_run:
        print("[!] DRY-RUN MODE ACTIVE: No changes will be executed.")

    orchestrator = Coyote(
        profile_path=args.profile,
        ndk_path=args.ndk,
        api_level=args.api_level,
        dry_run=args.dry_run,
    )

    if not args.skip_adb:
        orchestrator.configure_adb_properties()

    orchestrator.generate_c_header()
    agent_bin = orchestrator.compile_agent()

    if not args.skip_deploy:
        orchestrator.deploy_and_execute(agent_bin)


if __name__ == "__main__":
    main()