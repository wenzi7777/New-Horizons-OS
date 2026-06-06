#!/usr/bin/env python3
from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
from typing import Any


PRODUCT = "New Horizons OS Arduino"
PROTOCOL = "NHO/Arduino/1"


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def build_manifest(*, firmware_path: Path, model: str, version: str, base_url: str, changelog_url: str = "") -> dict[str, Any]:
    firmware_path = Path(firmware_path)
    if not firmware_path.is_file():
        raise FileNotFoundError(str(firmware_path))
    base = str(base_url).rstrip("/")
    manifest = {
        "product": PRODUCT,
        "protocol": PROTOCOL,
        "model": model,
        "latest": version,
        "firmware": {
            "url": f"{base}/{firmware_path.name}",
            "sha256": sha256_file(firmware_path),
            "size": firmware_path.stat().st_size,
        },
    }
    if changelog_url:
        manifest["changelog_url"] = str(changelog_url).strip()
    return manifest


def write_manifest(*, output_path: Path, firmware_path: Path, model: str, version: str, base_url: str, changelog_url: str = "") -> dict[str, Any]:
    manifest = build_manifest(
        firmware_path=firmware_path,
        model=model,
        version=version,
        base_url=base_url,
        changelog_url=changelog_url,
    )
    output_path = Path(output_path)
    output_path.parent.mkdir(parents=True, exist_ok=True)
    output_path.write_text(json.dumps(manifest, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    return manifest


def main() -> None:
    parser = argparse.ArgumentParser(description="Generate New Horizons Arduino OTA manifest JSON.")
    parser.add_argument("--firmware", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--model", default="VD-CTL/R v1.0.F 2026.4")
    parser.add_argument("--version", required=True)
    parser.add_argument("--base-url", required=True)
    parser.add_argument("--changelog-url", default="")
    args = parser.parse_args()
    write_manifest(
        output_path=args.output,
        firmware_path=args.firmware,
        model=args.model,
        version=args.version,
        base_url=args.base_url,
        changelog_url=args.changelog_url,
    )


if __name__ == "__main__":
    main()
