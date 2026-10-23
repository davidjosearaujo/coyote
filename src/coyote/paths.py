"""Filesystem locations for Coyote.

Read-only assets (the C sources, the JS agent, the profile database and the
example profiles) ship *inside* the installed package. Everything Coyote
*writes* — the downloaded Frida-GumJS devkit and the per-build output — lives
under a user cache directory so the tool keeps working when it is installed
read-only (e.g. via ``pipx``).

Override the cache location with ``$COYOTE_CACHE_DIR``; otherwise it follows
``$XDG_CACHE_HOME`` and falls back to ``~/.cache``.
"""
import os
from pathlib import Path

# --- Read-only package data (shipped in the wheel) ---
PACKAGE_DIR = Path(__file__).resolve().parent
AGENT_SRC = PACKAGE_DIR / "agent_src"
PROFILES_DIR = PACKAGE_DIR / "profiles"


def _cache_root() -> Path:
    """User cache dir for Coyote's writable artifacts (XDG-aware)."""
    override = os.environ.get("COYOTE_CACHE_DIR")
    if override:
        return Path(override).expanduser()
    xdg = os.environ.get("XDG_CACHE_HOME")
    base = Path(xdg).expanduser() if xdg else Path.home() / ".cache"
    return base / "coyote"


CACHE_ROOT = _cache_root()
# Downloaded Frida-GumJS devkit: frida-gumjs.h + libfrida-gumjs.a.
DEVKIT_DIR = CACHE_ROOT / "devkit"
# Per-build output: config.h, agent.h, _agent.js, libcoyote.so.
BUILD_DIR = CACHE_ROOT / "build"
