#!/usr/bin/env python3
from __future__ import annotations

import argparse
import shutil
import subprocess
import sys
from pathlib import Path

STALE_CHANNEL_FILES = {
    "minimal": ["device_state/network_config.json"],
}


def run(cmd: list[str]) -> None:
    print("+", " ".join(cmd))
    subprocess.run(cmd, check=True)


def remote_mkdir(port: str, remote_dir: str) -> None:
    if not remote_dir or remote_dir == ".":
        return
    cmd = ["mpremote", "connect", port, "fs", "mkdir", remote_dir]
    print("+", " ".join(cmd))
    result = subprocess.run(cmd, capture_output=True, text=True)
    if result.stdout:
        print(result.stdout, end="")
    if result.stderr:
        print(result.stderr, end="", file=sys.stderr)
    if result.returncode == 0:
        return
    if "File exists" in result.stdout or "File exists" in result.stderr:
        return
    raise subprocess.CalledProcessError(result.returncode, cmd)


def remote_copy(port: str, local_path: Path, remote_path: str) -> None:
    run(["mpremote", "connect", port, "fs", "cp", "-f", str(local_path), f":{remote_path}"])


def remote_remove(port: str, remote_path: str) -> None:
    cmd = ["mpremote", "connect", port, "fs", "rm", remote_path]
    print("+", " ".join(cmd))
    result = subprocess.run(cmd, capture_output=True, text=True)
    if result.stdout:
        print(result.stdout, end="")
    if result.stderr:
        print(result.stderr, end="", file=sys.stderr)
    if result.returncode == 0:
        return
    text = (result.stdout or "") + (result.stderr or "")
    if "No such file" in text or "ENOENT" in text:
        return
    raise subprocess.CalledProcessError(result.returncode, cmd)


def collect_files(root: Path) -> list[Path]:
    files = []
    for path in root.rglob("*"):
        if not path.is_file():
            continue
        rel_parts = path.relative_to(root).parts
        if "__pycache__" in rel_parts or path.suffix == ".pyc":
            continue
        files.append(path)
    return sorted(files)


def upload_tree(port: str, source_root: Path, remote_root: str = "") -> None:
    files = collect_files(source_root)
    dirs = sorted(
        {
            str(p.parent.relative_to(source_root)).replace("\\", "/")
            for p in files
            if p.parent != source_root
        },
        key=lambda item: (item.count("/"), item),
    )

    for remote_dir in dirs:
        target = remote_dir if not remote_root else f"{remote_root.rstrip('/')}/{remote_dir}"
        remote_mkdir(port, target)

    for path in files:
        rel = str(path.relative_to(source_root)).replace("\\", "/")
        target = rel if not remote_root else f"{remote_root.rstrip('/')}/{rel}"
        remote_copy(port, path, target)


def stale_device_paths(channel: str, channel_only: bool) -> list[str]:
    if channel_only:
        return []
    return list(STALE_CHANNEL_FILES.get(channel, []))


def remove_stale_channel_files(port: str, channel: str, channel_only: bool = False) -> None:
    for path in stale_device_paths(channel, channel_only):
        remote_remove(port, path)


def main() -> int:
    parser = argparse.ArgumentParser(description="Upload New Horizons OS files to a MicroPython board via mpremote.")
    parser.add_argument("--port", default="/dev/cu.usbserial-10", help="Serial port of the MicroPython board")
    parser.add_argument("--channel", choices=["minimal", "full"], default="minimal", help="Channel files to upload")
    parser.add_argument("--channel-only", action="store_true", help="Only upload channel files, skip immutable layer")
    parser.add_argument("--no-reset", action="store_true", help="Do not soft-reset after upload")
    args = parser.parse_args()

    if shutil.which("mpremote") is None:
        print("mpremote 未安裝。請先執行：python3 -m pip install --user mpremote", file=sys.stderr)
        return 2

    repo_root = Path(__file__).resolve().parents[2]
    immutable_root = repo_root / "device" / "immutable"
    channel_root = repo_root / "device" / "channels" / args.channel / "files"

    if not immutable_root.exists():
        print(f"找不到 immutable 目錄: {immutable_root}", file=sys.stderr)
        return 2
    if not channel_root.exists():
        print(f"找不到 channel 目錄: {channel_root}", file=sys.stderr)
        return 2

    if not args.channel_only:
        print(f"Uploading immutable layer from {immutable_root}")
        upload_tree(args.port, immutable_root)

    remove_stale_channel_files(args.port, args.channel, channel_only=args.channel_only)
    print(f"Uploading channel '{args.channel}' from {channel_root}")
    upload_tree(args.port, channel_root)

    if not args.no_reset:
        run(["mpremote", "connect", args.port, "soft-reset"])

    print("Upload complete.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
