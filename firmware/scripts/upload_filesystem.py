#!/usr/bin/env python3
from __future__ import annotations

import argparse
import shutil
import subprocess
import sys
from pathlib import Path

RAW_COPY_CHUNK_SIZE = 2048
DEVICE_OS_DIR = "nhos"

STALE_TARGET_FILES = {
    "recovery": ["device_state/network_config.json"],
}


class UploadLayer:
    def __init__(self, name: str, source_root: Path, remote_root: str):
        self.name = name
        self.source_root = source_root
        self.remote_root = remote_root


def run(cmd: list[str], display: str | None = None) -> None:
    print("+", display or " ".join(cmd))
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
    cmd = ["mpremote", "connect", port, "fs", "cp", "-f", str(local_path), f":{remote_path}"]
    print("+", " ".join(cmd))
    result = subprocess.run(cmd, capture_output=True, text=True)
    if result.returncode == 0:
        if result.stdout:
            print(result.stdout, end="")
        if result.stderr:
            print(result.stderr, end="", file=sys.stderr)
        return
    text = (result.stdout or "") + (result.stderr or "")
    missing_stat = "has no attribute 'stat'" in text
    if result.stdout:
        print(result.stdout, end="")
    if result.stderr and not missing_stat:
        print(result.stderr, end="", file=sys.stderr)
    if not missing_stat:
        raise subprocess.CalledProcessError(result.returncode, cmd)
    print(f"mpremote fs cp cannot use remote os.stat; using raw copy for {remote_path}")
    remote_copy_raw(port, local_path, remote_path)


def remote_exec(port: str, code: str, display: str) -> None:
    run(["mpremote", "connect", port, "exec", code], display=display)


def remote_copy_raw(port: str, local_path: Path, remote_path: str) -> None:
    remote_exec(
        port,
        f"f=open({remote_path!r},'wb');f.close()",
        f"mpremote connect {port} exec <truncate {remote_path}>",
    )
    with local_path.open("rb") as handle:
        offset = 0
        while True:
            chunk = handle.read(RAW_COPY_CHUNK_SIZE)
            if not chunk:
                break
            code = f"f=open({remote_path!r},'ab');f.write({chunk!r});f.close()"
            remote_exec(
                port,
                code,
                f"mpremote connect {port} exec <write {remote_path} +{len(chunk)} bytes @{offset}>",
            )
            offset += len(chunk)


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
        if any(part.startswith(".") for part in rel_parts):
            continue
        if "__pycache__" in rel_parts or path.suffix == ".pyc":
            continue
        files.append(path)
    return sorted(files)


def upload_tree(port: str, source_root: Path, remote_root: str = "") -> None:
    files = collect_files(source_root)
    if remote_root:
        remote_mkdir(port, remote_root.rstrip("/"))

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


def stale_device_paths(target: str, target_only: bool) -> list[str]:
    if target_only:
        return []
    if target == "all":
        return list(STALE_TARGET_FILES.get("recovery", []))
    return list(STALE_TARGET_FILES.get(target, []))


def remove_stale_target_files(port: str, target: str, target_only: bool = False) -> None:
    for path in stale_device_paths(target, target_only):
        remote_remove(port, path)


def target_layers(target: str, target_only: bool = False, repo_root: Path | None = None) -> list[UploadLayer]:
    repo_root = repo_root or Path(__file__).resolve().parents[2]
    layers = {
        "root": UploadLayer("root", repo_root / "device" / "root", ""),
        "recovery": UploadLayer("recovery", repo_root / "device" / "recovery", "recovery"),
        "os": UploadLayer("os", repo_root / "device" / "os", DEVICE_OS_DIR),
    }
    if target == "recovery":
        return [layers["recovery"]] if target_only else [layers["root"], layers["recovery"]]
    if target == "os":
        return [layers["os"]]
    if target == "all":
        return [layers["recovery"], layers["os"]] if target_only else [layers["root"], layers["recovery"], layers["os"]]
    raise ValueError("unsupported target")


def main() -> int:
    parser = argparse.ArgumentParser(description="Upload New Horizons OS files to a MicroPython board via mpremote.")
    parser.add_argument("--port", default="/dev/cu.usbserial-10", help="Serial port of the MicroPython board")
    parser.add_argument("--target", choices=["recovery", "os", "all"], required=True, help="Filesystem target to upload")
    parser.add_argument("--target-only", action="store_true", help="Only upload the selected target, skip root layer")
    parser.add_argument("--no-reset", action="store_true", help="Do not soft-reset after upload")
    args = parser.parse_args()

    if shutil.which("mpremote") is None:
        print("mpremote 未安裝。請先執行：python3 -m pip install --user mpremote", file=sys.stderr)
        return 2

    repo_root = Path(__file__).resolve().parents[2]
    target = args.target
    target_only = args.target_only
    try:
        layers = target_layers(target, target_only=target_only, repo_root=repo_root)
    except ValueError as exc:
        print(str(exc), file=sys.stderr)
        return 2

    for layer in layers:
        if not layer.source_root.exists():
            print(f"找不到 {layer.name} 目錄: {layer.source_root}", file=sys.stderr)
            return 2

    remove_stale_target_files(args.port, target, target_only=target_only)

    for layer in layers:
        remote_label = "/" + layer.remote_root.strip("/") if layer.remote_root else "/"
        print(f"Uploading target '{layer.name}' from {layer.source_root} to {remote_label}")
        upload_tree(args.port, layer.source_root, layer.remote_root)

    if not args.no_reset:
        run(["mpremote", "connect", args.port, "soft-reset"])

    print("Upload complete.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
