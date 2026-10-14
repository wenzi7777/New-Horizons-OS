#!/usr/bin/env python3
"""Command-surface regression check against a real device, over UDP.

Sends every command that is safe to send to a device in NORMAL mode and
records, per command, whether it succeeded, its message/error, and the SHAPE
of its data (keys and value types, never values). Record a baseline on one
firmware, then compare another against it:

    python3 tools/command_smoke.py 192.168.1.6 --baseline base.json
    python3 tools/command_smoke.py 192.168.1.6 --compare base.json

Nothing here changes the device:
  - read-only commands are sent as-is;
  - maintenance-only commands are sent in normal mode with ids/paths that do
    not exist, so they must be refused (every one of them checks the mode
    before touching anything);
  - setters are sent with the values the device already has.

Built for the ControlServer command-registry refactor (v1.15.0), where the
dispatch changed but no command may change what it answers.
"""
from __future__ import annotations

import argparse
import json
import socket
import sys
import time
from typing import Any

UDP_PORT = 13250

READ_ONLY = [
    ("status", {}), ("query", {}), ("sensor_sample", {}), ("memory_status", {}),
    ("task_list", {}), ("service_list", {}), ("scan_health", {}), ("storage_status", {}),
    ("battery_status", {}), ("clock_status", {}), ("health", {}), ("capabilities", {}),
    ("config_schema", {}), ("config_get", {}), ("app_list", {}), ("app_view", {}),
    ("app_events", {"since_seq": 0, "limit": 4}), ("app_list_packages", {}),
    ("app_verify", {"id": "smoke_missing"}), ("calibration_status", {}),
    ("calibration_dump_tare", {}), ("calibration_dump_level", {"level": 123456}),
    ("calibration_dump_fit", {}), ("file_list", {"scope": "user"}),
    ("file_read_begin", {"scope": "user", "path": "smoke_missing.txt"}),
    ("log_tail", {"max_lines": 3}), ("dmesg", {}), ("crash_log", {}),
]

# Refused in normal mode before any side effect; arguments name nothing real.
MAINTENANCE_ONLY = [
    ("app_install", {"path": "apps/smoke_missing.nha", "sha256": "0" * 64}),
    ("app_uninstall", {"id": "smoke_missing"}),
    ("app_activate", {"id": "smoke_missing", "slot": 0}),
    ("app_deactivate", {"id": "smoke_missing", "slot": 0}),
    ("app_reindex", {}),
    ("calibration_session_begin", {}), ("calibration_session_abort", {}),
    ("calibration_session_commit", {}), ("calibration_clear_profile", {}),
    ("calibration_set_fit", {}), ("calibration_set_reference", {"level": 123456, "reference": 1}),
    ("calibration_delete_level", {"level": 123456}), ("calibration_capture_tare", {"duration_ms": 1}),
    ("calibration_capture_cell", {"row": 0, "col": 0, "level": 123456, "duration_ms": 1}),
    ("calibration_capture_all", {"level": 123456, "duration_ms": 1}),
    ("file_write_begin", {"scope": "user", "path": "smoke_missing.txt", "size": 1}),
    ("file_write_chunk", {"scope": "user", "path": "smoke_missing.txt", "offset": 0, "data": ""}),
    ("file_write_finish", {"scope": "user", "path": "smoke_missing.txt"}),
    ("file_delete", {"scope": "user", "path": "smoke_missing.txt"}),
    # v1.15.0 makes this maintenance-only (it was a no-op in normal mode).
    ("exit_maintenance", {}),
]

UNKNOWN = [("smoke_no_such_command", {})]

# Commands whose answer is allowed to differ between firmware versions.
ALLOWED_DIFFS = {"exit_maintenance", "capabilities", "commands"}


class Device:
    def __init__(self, host: str) -> None:
        self.host = host
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.sock.bind(("", 0))
        self.sock.settimeout(0.3)
        self.seq = 0

    def command(self, payload: dict[str, Any], timeout: float = 20.0) -> dict[str, Any]:
        self.seq += 1
        request_id = f"smoke{self.seq}"
        frame = {"type": "command", "seq": self.seq, "request_id": request_id, "payload": payload}
        self.sock.sendto(json.dumps(frame).encode(), (self.host, UDP_PORT))
        chunks: dict[int, str] = {}
        total = None
        deadline = time.time() + timeout
        while time.time() < deadline:
            try:
                data, _ = self.sock.recvfrom(4096)
            except socket.timeout:
                continue
            try:
                msg = json.loads(data)
            except ValueError:
                continue
            if msg.get("request_id") != request_id:
                continue
            if msg.get("type") == "result":
                return msg.get("payload") or {}
            if msg.get("type") == "result_chunk":
                chunks[int(msg["chunk"])] = msg["data"]
                total = int(msg["chunks"])
                if len(chunks) == total:
                    return json.loads("".join(chunks[i] for i in range(total)))
        raise TimeoutError(f"{payload.get('command')}: no complete result")


def shape(value: Any) -> Any:
    if isinstance(value, dict):
        return {k: shape(v) for k, v in sorted(value.items())}
    if isinstance(value, list):
        return [shape(value[0])] if value else []
    if isinstance(value, bool):
        return "bool"
    if isinstance(value, (int, float)):
        return "number"
    if value is None:
        return "null"
    return "str"


def record(dev: Device, name: str, args: dict[str, Any]) -> dict[str, Any]:
    reply = dev.command({"command": name, "quiet": True, **args})
    return {
        "ok": bool(reply.get("ok")),
        "message": reply.get("message") or "",
        "error": reply.get("error") or "",
        "shape": shape(reply.get("data")),
    }


def current_setters(status: dict[str, Any]) -> list[tuple[str, dict[str, Any]]]:
    """Setters that re-apply what the device already has."""
    cfg = status.get("config") or {}
    log = status.get("logging") or {}
    ota = status.get("ota") or {}
    timing = ((status.get("runtime") or {}).get("scan_timing")) or {}
    filt = status.get("filter") or cfg.get("filter") or {}
    setters: list[tuple[str, dict[str, Any]]] = [
        ("set_log", {"enabled": bool(log.get("enabled", True)), "level": log.get("level", "error"),
                     "mode": log.get("mode", "standard"), "max_bytes": log.get("max_bytes", 12288)}),
        ("set_ota_config", {"auto_apply_on_boot": bool(ota.get("auto_apply_on_boot", True)),
                            **({"channel": ota["channel"]} if "channel" in ota else {})}),
        ("config_set", {"path": "log.level", "value": log.get("level", "error")}),
        ("clock_sync", {}),
    ]
    if timing:
        setters.append(("set_scan_timing", {"target_fps": timing.get("target_fps", 60),
                                            "settle_us": timing.get("settle_us", 20),
                                            "send_every_n_frames": timing.get("send_every_n_frames", 1)}))
    if filt:
        setters.append(("set_filter", {"enabled": bool(filt.get("enabled", False)),
                                       "median": filt.get("median", 1), "alpha": filt.get("alpha", 1.0)}))
    return setters


def run(host: str) -> dict[str, Any]:
    dev = Device(host)
    status = dev.command({"command": "status", "quiet": True}).get("data") or {}
    if status.get("mode") != "normal":
        sys.exit(f"device must be in normal mode (is {status.get('mode')!r})")
    out: dict[str, Any] = {"firmware": status.get("firmware_version"), "commands": {}}
    groups = [("read", READ_ONLY), ("maintenance", MAINTENANCE_ONLY), ("unknown", UNKNOWN),
              ("setter", current_setters(status))]
    for group, items in groups:
        for name, args in items:
            try:
                entry = record(dev, name, args)
            except TimeoutError as exc:
                entry = {"ok": False, "message": "", "error": f"timeout: {exc}", "shape": None}
            entry["group"] = group
            out["commands"][name] = entry
            time.sleep(0.15)
    return out


def check_expectations(result: dict[str, Any]) -> list[str]:
    problems = []
    for name, entry in result["commands"].items():
        if entry["group"] == "maintenance" and name != "exit_maintenance":
            if entry["error"] != "maintenance_required":
                problems.append(f"{name}: expected maintenance_required, got {entry['error'] or entry['message']!r}")
        if entry["group"] == "unknown" and entry["error"] != "unknown_command":
            problems.append(f"{name}: expected unknown_command, got {entry['error']!r}")
        if entry["error"].startswith("timeout"):
            problems.append(f"{name}: {entry['error']}")
    return problems


def compare(base: dict[str, Any], now: dict[str, Any]) -> list[str]:
    diffs = []
    for name, before in base["commands"].items():
        after = now["commands"].get(name)
        if after is None:
            diffs.append(f"{name}: missing from the new run")
            continue
        for key in ("ok", "message", "error", "shape"):
            if before[key] != after[key]:
                tag = "allowed" if name in ALLOWED_DIFFS else "DIFF"
                detail = "" if key == "shape" else f" {before[key]!r} -> {after[key]!r}"
                diffs.append(f"[{tag}] {name}.{key}{detail}")
    return diffs


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("host")
    mode = parser.add_mutually_exclusive_group(required=True)
    mode.add_argument("--baseline", help="write the run to this file")
    mode.add_argument("--compare", help="compare the run against this baseline")
    args = parser.parse_args()

    result = run(args.host)
    problems = check_expectations(result)
    print(f"firmware {result['firmware']}: {len(result['commands'])} commands")
    for p in problems:
        print(f"  EXPECTATION: {p}")
    if args.baseline:
        with open(args.baseline, "w", encoding="utf-8") as handle:
            json.dump(result, handle, indent=1, sort_keys=True)
        print(f"baseline written to {args.baseline}")
        return 1 if problems else 0
    with open(args.compare, encoding="utf-8") as handle:
        base = json.load(handle)
    diffs = compare(base, result)
    print(f"compared with {base['firmware']}:")
    for d in diffs:
        print(f"  {d}")
    hard = [d for d in diffs if d.startswith("[DIFF]") or d.endswith("new run")]
    print("OK" if not hard and not problems else f"FAILED ({len(hard)} diffs, {len(problems)} expectation problems)")
    return 1 if hard or problems else 0


if __name__ == "__main__":
    sys.exit(main())
