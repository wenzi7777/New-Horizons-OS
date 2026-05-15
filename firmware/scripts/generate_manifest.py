#!/usr/bin/env python3
import argparse
import hashlib
import json
from pathlib import Path


def sha256_file(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as fh:
        while True:
            chunk = fh.read(65536)
            if not chunk:
                break
            h.update(chunk)
    return h.hexdigest()


def should_include(path: Path, root: Path) -> bool:
    rel_parts = path.relative_to(root).parts
    if "__pycache__" in rel_parts:
        return False
    if path.suffix == ".pyc":
        return False
    if rel_parts and rel_parts[0] == ".device" and path.suffix == ".json":
        return False
    return True


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--channel", required=True, choices=["minimal", "full"])
    parser.add_argument("--version", required=True)
    parser.add_argument("--repo-root", required=True)
    parser.add_argument("--firmware-name", default="New Horizons OS")
    args = parser.parse_args()

    repo_root = Path(args.repo_root).resolve()
    files_root = repo_root / "device" / "channels" / args.channel / "files"
    manifest_path = repo_root / "device" / "channels" / args.channel / "manifest.json"
    base_url = f"https://raw.githubusercontent.com/wenzi7777/New-Horizons-OS/{args.version}/device/channels/{args.channel}/files"

    files = []
    for path in sorted(p for p in files_root.rglob("*") if p.is_file() and should_include(p, files_root)):
        rel = path.relative_to(files_root).as_posix()
        files.append({
            "path": rel,
            "sha256": sha256_file(path),
            "size": path.stat().st_size,
            "kind": "config" if rel.endswith(".json") else "code",
            "reboot_required": rel.endswith(".py"),
        })

    manifest = {
        "manifest_version": 1,
        "firmware_name": args.firmware_name,
        "firmware_version": args.version,
        "channel": args.channel,
        "base_url": base_url,
        "files": files,
    }
    manifest_path.write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")


if __name__ == "__main__":
    main()
