"""ControlServer's command surface, as the tests read it.

Since v1.15.0 the commands are a table (ControlCommandTable.cpp) of handlers
spread over ControlCommands*.cpp, with the transports left in
ControlServer.cpp. Tests that used to grep one file read through here.
"""
from __future__ import annotations

import re
from pathlib import Path

ARDUINO_ROOT = Path(__file__).resolve().parents[1] / "firmware" / "newhorizons_os"
_FILES = ["ControlServer.cpp", "ControlCommandTable.cpp"] + sorted(
    p.name for p in ARDUINO_ROOT.glob("ControlCommands*.cpp")
)
_ROW = re.compile(
    r'\{"(?P<name>[a-z_0-9]+)", &ControlServer::(?P<handler>cmd\w+), (?P<flags>[^,]+),\s*"(?P<summary>(?:[^"\\]|\\.)*)"\}'
)


def control_source() -> str:
    """Every ControlServer source file, concatenated."""
    return "\n".join((ARDUINO_ROOT / name).read_text(encoding="utf-8") for name in _FILES)


def command_rows() -> list[dict[str, object]]:
    """The command table, in order: name, handler, flag names, summary."""
    table = (ARDUINO_ROOT / "ControlCommandTable.cpp").read_text(encoding="utf-8")
    rows = []
    for m in _ROW.finditer(table):
        flags = [] if m.group("flags").strip() == "0" else [f.strip() for f in m.group("flags").split("|")]
        rows.append({"name": m.group("name"), "handler": m.group("handler"), "flags": flags,
                     "summary": m.group("summary")})
    return rows


def command_names() -> list[str]:
    return [row["name"] for row in command_rows()]


def flagged(flag: str) -> set[str]:
    return {row["name"] for row in command_rows() if flag in row["flags"]}


def handler_body(command: str) -> str:
    """The handler function registered for `command`, signature to closing brace."""
    handler = next(row["handler"] for row in command_rows() if row["name"] == command)
    source = control_source()
    start = source.index(f"String ControlServer::{handler}(const CommandContext& ctx) {{")
    return source[start:source.index("\n}\n", start) + 2]
