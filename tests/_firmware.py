"""Where a firmware source file lives.

Since v1.15.0 the code the firmware shares with the Hub is a library,
firmware/libraries/nhos_core/src; everything else stays in the sketch,
firmware/newhorizons_os. Tests name a file and get it from whichever holds it.
"""
from __future__ import annotations

from pathlib import Path

REPO = Path(__file__).resolve().parents[1]
ARDUINO_ROOT = REPO / "firmware" / "newhorizons_os"
CORE_ROOT = REPO / "firmware" / "libraries" / "nhos_core" / "src"


def firmware_file(name: str) -> Path:
    sketch = ARDUINO_ROOT / name
    return sketch if sketch.exists() or not (CORE_ROOT / name).exists() else CORE_ROOT / name


def read_firmware(name: str) -> str:
    return firmware_file(name).read_text(encoding="utf-8")


def firmware_sources(*suffixes: str) -> list[Path]:
    """Every source file of the build: the sketch and the library."""
    suffixes = suffixes or (".h", ".cpp", ".ino")
    return sorted(p for root in (ARDUINO_ROOT, CORE_ROOT) for p in root.iterdir() if p.suffix in suffixes)
