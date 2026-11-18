"""End-to-end regression test for Coyote against the Border Patrol test app.

This builds the agent, deploys it onto a connected device/emulator via the
``coyote`` CLI, launches Border Patrol, dumps its UI, and asserts that every
validation check passed. It is the automated safety net for timing/ordering
regressions (e.g. a hook that installs too late) that otherwise only surface as
a lower pass count on a manual run.

It is an integration test: it needs a connected, rooted device/emulator, the
Android NDK, and frida-tools. When any prerequisite is missing the test is
skipped (never failed), so it stays green in environments that can't run it.

Run it with::

    pytest test/test_border_patrol.py -v

Relevant environment variables (all optional):

* ``ANDROID_SERIAL``     – target a specific device (as in ``adb devices``).
* ``ANDROID_NDK_HOME``   – NDK path passed to the build (else ``--ndk`` default).
* ``COYOTE_TEST_PROFILE``– profile JSON (default: the bundled test profile).
* ``COYOTE_TEST_APK``    – prebuilt Border Patrol APK to install (else it is
                           built with gradle, or an already-installed copy is
                           used).
* ``COYOTE_TEST_TIMEOUT``– seconds to wait for the report to render (default 60).
"""

from __future__ import annotations

import importlib.util
import os
import re
import shutil
import subprocess
import sys
import time
import xml.etree.ElementTree as ET
from pathlib import Path

import pytest

PACKAGE = "com.border.patrol"
TEST_DIR = Path(__file__).resolve().parent
REPO_ROOT = TEST_DIR.parent
DEFAULT_PROFILE = TEST_DIR / "src" / "main" / "assets" / "test_profile.json"
APK_REL = Path("build") / "outputs" / "apk" / "debug" / "borderpatrol-debug.apk"

# Border Patrol's final summary line (see MainActivity): on full success it is
# "✓ All <N> system checks passed successfully!", otherwise
# "⚠ <passed> of <total> checks passed".
PASS_RE = re.compile(r"All (\d+) system checks passed successfully")
PARTIAL_RE = re.compile(r"(\d+) of (\d+) checks passed")


# --------------------------------------------------------------------------- #
# ADB / environment helpers
# --------------------------------------------------------------------------- #
def _adb_base() -> list[str]:
    adb = shutil.which("adb")
    if not adb:
        pytest.skip("adb not found on PATH")
    base = [adb]
    serial = os.environ.get("ANDROID_SERIAL")
    if serial:
        base += ["-s", serial]
    return base


def _adb(*args: str, timeout: int = 60, check: bool = True) -> subprocess.CompletedProcess:
    proc = subprocess.run(
        _adb_base() + list(args),
        capture_output=True,
        text=True,
        timeout=timeout,
    )
    if check and proc.returncode != 0:
        raise AssertionError(
            f"adb {' '.join(args)} failed (rc={proc.returncode}): {proc.stderr.strip()}"
        )
    return proc


def _connected_devices() -> list[str]:
    adb = shutil.which("adb")
    if not adb:
        return []
    try:
        out = subprocess.run(
            [adb, "devices"], capture_output=True, text=True, timeout=30
        ).stdout
    except Exception:
        return []
    devices = []
    for line in out.splitlines()[1:]:
        parts = line.split()
        if len(parts) == 2 and parts[1] == "device":
            devices.append(parts[0])
    return devices


def _is_rooted() -> bool:
    try:
        out = _adb("shell", "su", "0", "id", "-u", timeout=20, check=False).stdout.strip()
    except Exception:
        return False
    return out.endswith("0")


# --------------------------------------------------------------------------- #
# Fixtures: skip early unless the whole chain is runnable
# --------------------------------------------------------------------------- #
@pytest.fixture(scope="module")
def device() -> str:
    devices = _connected_devices()
    if not devices:
        pytest.skip("no connected adb device/emulator")
    serial = os.environ.get("ANDROID_SERIAL")
    if serial and serial not in devices:
        pytest.skip(f"ANDROID_SERIAL={serial} is not among connected devices {devices}")
    if not serial and len(devices) > 1:
        pytest.skip(
            f"multiple devices connected {devices}; set ANDROID_SERIAL to pick one"
        )
    if not _is_rooted():
        pytest.skip("device is not rooted (su required); cannot run Coyote")
    return serial or devices[0]


@pytest.fixture(scope="module")
def ndk_path() -> str:
    ndk = os.environ.get("ANDROID_NDK_HOME")
    if not ndk or not Path(ndk).is_dir():
        pytest.skip("ANDROID_NDK_HOME is unset or not a directory; needed to build the agent")
    return ndk


@pytest.fixture(scope="module")
def frida_tools() -> None:
    if importlib.util.find_spec("frida_tools") is None:
        pytest.skip("frida-tools not installed; needed to build the JS agent")


@pytest.fixture(scope="module")
def profile() -> Path:
    path = Path(os.environ.get("COYOTE_TEST_PROFILE", DEFAULT_PROFILE))
    if not path.is_file():
        pytest.skip(f"test profile not found: {path}")
    return path


@pytest.fixture(scope="module")
def border_patrol_installed(device: str) -> None:
    """Ensure Border Patrol is installed: use an explicit/prebuilt APK, build it
    with gradle if possible, or accept an already-installed copy."""
    apk = os.environ.get("COYOTE_TEST_APK")
    apk_path = Path(apk) if apk else (TEST_DIR / APK_REL)

    if not apk_path.is_file() and shutil.which("gradle"):
        build = subprocess.run(
            ["gradle", "assembleDebug"],
            cwd=TEST_DIR, capture_output=True, text=True, timeout=1200,
        )
        if build.returncode != 0:
            pytest.skip(f"gradle assembleDebug failed:\n{build.stdout[-2000:]}\n{build.stderr[-2000:]}")

    if apk_path.is_file():
        _adb("install", "-r", "-g", str(apk_path), timeout=300)
        return

    # Fall back to whatever is already installed on the device.
    listed = _adb("shell", "pm", "list", "packages", PACKAGE, check=False).stdout
    if PACKAGE not in listed:
        pytest.skip(
            "Border Patrol APK not found and not installed. Build it "
            "(gradle assembleDebug in test/) or set COYOTE_TEST_APK."
        )


# --------------------------------------------------------------------------- #
# UI scraping
# --------------------------------------------------------------------------- #
def _dump_ui_text() -> str:
    """Dump the current screen via uiautomator and return all node text joined."""
    remote = "/sdcard/coyote_window_dump.xml"
    _adb("shell", "rm", "-f", remote, check=False)
    dump = _adb("shell", "uiautomator", "dump", remote, timeout=60, check=False)
    if "ERROR" in dump.stdout or "null root node" in dump.stdout.lower():
        return ""
    xml = _adb("shell", "cat", remote, timeout=60, check=False).stdout
    if "<hierarchy" not in xml:
        return ""
    try:
        root = ET.fromstring(xml)
    except ET.ParseError:
        return ""
    texts = [node.attrib.get("text", "") for node in root.iter("node")]
    return "\n".join(t for t in texts if t)


def _launch_report() -> str:
    """Poll the Border Patrol UI until its summary line appears, returning it."""
    timeout = int(os.environ.get("COYOTE_TEST_TIMEOUT", "60"))
    deadline = time.time() + timeout
    last = ""
    while time.time() < deadline:
        screen = _dump_ui_text()
        if screen:
            last = screen
            if PASS_RE.search(screen) or PARTIAL_RE.search(screen) or "Error generating" in screen:
                return screen
        time.sleep(2)
    return last


# --------------------------------------------------------------------------- #
# The test
# --------------------------------------------------------------------------- #
def test_border_patrol_all_checks_pass(
    device, ndk_path, frida_tools, profile, border_patrol_installed
):
    """Run the target under Coyote and assert Border Patrol reports a full pass."""
    cmd = [
        sys.executable, "-m", "coyote.cli",
        "-p", str(profile),
        "-t", PACKAGE,
        "--ndk", ndk_path,
    ]
    if os.environ.get("ANDROID_SERIAL"):
        cmd += ["-s", os.environ["ANDROID_SERIAL"]]

    env = dict(os.environ)
    env["PYTHONPATH"] = os.pathsep.join(
        [str(REPO_ROOT / "src"), env.get("PYTHONPATH", "")]
    ).rstrip(os.pathsep)

    run = subprocess.run(
        cmd, cwd=REPO_ROOT, capture_output=True, text=True, timeout=1800, env=env
    )
    assert run.returncode == 0, (
        f"coyote run failed (rc={run.returncode}):\n"
        f"--- stdout ---\n{run.stdout[-4000:]}\n--- stderr ---\n{run.stderr[-4000:]}"
    )

    report = _launch_report()
    assert report, (
        "Border Patrol UI never rendered a report (no summary line scraped). "
        "The app may have crashed or not launched."
    )
    assert "Error generating" not in report, f"Border Patrol errored:\n{report}"

    if PASS_RE.search(report):
        return  # full pass — the happy path

    partial = PARTIAL_RE.search(report)
    if partial:
        passed, total = partial.group(1), partial.group(2)
        pytest.fail(
            f"Border Patrol: only {passed}/{total} checks passed (expected all).\n"
            f"Full report text:\n{report}"
        )
    pytest.fail(f"Could not parse Border Patrol summary from:\n{report}")
