#!/usr/bin/env python3
from __future__ import annotations

import argparse
import os
import shutil
import subprocess
import sys
from pathlib import Path


def should_include(path: Path, root: Path) -> bool:
    rel_parts = path.relative_to(root).parts
    if any(part.startswith(".") for part in rel_parts):
        return False
    if "__pycache__" in rel_parts:
        return False
    if path.suffix == ".pyc":
        return False
    if rel_parts and rel_parts[-1] == "manifest.json":
        return False
    return True


def find_default_mpy_cross(repo_root: Path) -> str:
    env = os.environ.get("MICROPY_MPYCROSS", "")
    if env:
        return env
    candidate = repo_root.parent / "third_party" / "micropython" / "mpy-cross" / "build" / "mpy-cross"
    if candidate.exists():
        return str(candidate)
    found = shutil.which("mpy-cross")
    return found or ""


def ensure_output_is_safe(source: Path, output: Path) -> None:
    if source == output:
        raise ValueError("output must be different from source")
    if source in output.parents:
        raise ValueError("output must not be inside source")
    if str(output) in ("", "/", "."):
        raise ValueError("unsafe output path")


def build_tree(source: Path, output: Path, mpy_cross: str) -> tuple[int, int]:
    ensure_output_is_safe(source, output)
    if not source.exists():
        raise FileNotFoundError(source)
    if not mpy_cross or not Path(mpy_cross).exists():
        raise FileNotFoundError("mpy-cross not found")
    if output.exists():
        shutil.rmtree(output)
    output.mkdir(parents=True, exist_ok=True)

    compiled = 0
    copied = 0
    files = sorted(p for p in source.rglob("*") if p.is_file() and should_include(p, source))
    for path in files:
        rel = path.relative_to(source)
        if path.suffix == ".py":
            dst = (output / rel).with_suffix(".mpy")
            dst.parent.mkdir(parents=True, exist_ok=True)
            subprocess.run([mpy_cross, "-o", str(dst), str(path)], check=True)
            compiled += 1
        else:
            dst = output / rel
            dst.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(path, dst)
            copied += 1
    return compiled, copied


def main() -> int:
    parser = argparse.ArgumentParser(description="Build a .mpy release tree from MicroPython source files.")
    parser.add_argument("--source", default="device/os", help="Source tree to compile.")
    parser.add_argument("--output", default="device/os_mpy", help="Output tree for release artifacts.")
    parser.add_argument("--mpy-cross", default="", help="Path to mpy-cross.")
    parser.add_argument("--repo-root", default="", help="Repository root for default path resolution.")
    args = parser.parse_args()

    repo_root = Path(args.repo_root).resolve() if args.repo_root else Path(__file__).resolve().parents[2]
    source = Path(args.source)
    output = Path(args.output)
    if not source.is_absolute():
        source = repo_root / source
    if not output.is_absolute():
        output = repo_root / output
    mpy_cross = args.mpy_cross or find_default_mpy_cross(repo_root)

    try:
        compiled, copied = build_tree(source.resolve(), output.resolve(), mpy_cross)
    except Exception as exc:
        print(str(exc), file=sys.stderr)
        return 2

    print("MPY release tree written to {}".format(output))
    print("compiled_py={} copied_assets={}".format(compiled, copied))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
