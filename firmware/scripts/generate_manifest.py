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
    if any(part.startswith(".") for part in rel_parts):
        return False
    if "__pycache__" in rel_parts:
        return False
    if path.suffix == ".pyc":
        return False
    if rel_parts and rel_parts[0] in (".device", "device_state"):
        return False
    if rel_parts and rel_parts[-1] == "manifest.json":
        return False
    return True


def target_paths(repo_root: Path, target: str) -> tuple[Path, Path, str, str]:
    if target == "os":
        files_root = repo_root / "device" / "os"
        manifest_path = files_root / "manifest.json"
        return files_root, manifest_path, "/nhos", "os"
    if target == "recovery":
        files_root = repo_root / "device" / "recovery"
        manifest_path = files_root / "manifest.json"
        return files_root, manifest_path, "/recovery", "recovery"
    raise ValueError("unsupported target")


def resolve_root(repo_root: Path, value: str | None, default: Path) -> Path:
    if not value:
        return default
    path = Path(value)
    if not path.is_absolute():
        path = repo_root / path
    return path.resolve()


def collect_deletes(delete_root: Path | None, suffixes: list[str]) -> list[str]:
    if delete_root is None or not suffixes:
        return []
    deletes = []
    for path in sorted(p for p in delete_root.rglob("*") if p.is_file() and should_include(p, delete_root)):
        rel = path.relative_to(delete_root).as_posix()
        if any(rel.endswith(suffix) for suffix in suffixes):
            deletes.append(rel)
    return deletes


def normalize_delete_path(value: str) -> str:
    rel = Path(value)
    if rel.is_absolute():
        raise ValueError(f"delete path must be relative: {value}")
    rel_text = rel.as_posix().strip("/")
    if not rel_text or rel_text == "." or ".." in rel.parts:
        raise ValueError(f"unsafe delete path: {value}")
    return rel_text


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--target", choices=["os", "recovery"], required=True)
    parser.add_argument("--version", required=True)
    parser.add_argument("--repo-root", required=True)
    parser.add_argument("--firmware-name", default="New Horizons OS")
    parser.add_argument("--source-root", default="", help="Optional source tree for files in this manifest.")
    parser.add_argument("--base-url-path", default="", help="Repo-relative path used in manifest base_url.")
    parser.add_argument("--delete-source-root", default="", help="Optional tree used to collect delete entries.")
    parser.add_argument(
        "--delete-suffix",
        action="append",
        default=[],
        help="Suffix to delete from --delete-source-root. May be passed more than once.",
    )
    parser.add_argument(
        "--delete-path",
        action="append",
        default=[],
        help="Additional repo-relative path to delete from target root. May be passed more than once.",
    )
    args = parser.parse_args()

    repo_root = Path(args.repo_root).resolve()
    target = args.target
    default_files_root, manifest_path, target_root, manifest_type = target_paths(repo_root, target)
    files_root = resolve_root(repo_root, args.source_root, default_files_root)
    base_url_path = (args.base_url_path or f"device/{target}").strip("/")
    base_url = f"https://raw.githubusercontent.com/wenzi7777/New-Horizons-OS/{args.version}/{base_url_path}"
    delete_root = resolve_root(repo_root, args.delete_source_root, default_files_root) if args.delete_source_root else None

    files = []
    for path in sorted(p for p in files_root.rglob("*") if p.is_file() and should_include(p, files_root)):
        rel = path.relative_to(files_root).as_posix()
        files.append({
            "path": rel,
            "sha256": sha256_file(path),
            "size": path.stat().st_size,
            "kind": "config" if rel.endswith(".json") else "code",
            "reboot_required": rel.endswith((".py", ".mpy")),
        })

    manifest = {
        "manifest_version": 1,
        "type": manifest_type,
        "firmware_name": args.firmware_name,
        "firmware_version": args.version,
        "version": args.version,
        "target_root": target_root,
        "base_url": base_url,
        "files": files,
    }
    deletes = collect_deletes(delete_root, args.delete_suffix)
    deletes.extend(normalize_delete_path(path) for path in args.delete_path)
    deletes = sorted(set(deletes))
    if deletes:
        manifest["delete"] = deletes
    manifest_path.write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")


if __name__ == "__main__":
    main()
