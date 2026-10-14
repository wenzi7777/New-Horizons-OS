#!/usr/bin/env python3
from __future__ import annotations

import argparse
import hashlib
import json
import re
import sys
from pathlib import Path
from typing import Any


PRODUCT = "NHOS Arduino"
PROTOCOL = "NHO/Arduino/1"
CHANNELS = ("stable", "beta")

_VERSION_RE = re.compile(r"^\s*[vV]?(\d+)(?:\.(\d+))?(?:\.(\d+))?[^-+]*(?:-([^+]*))?(?:\+.*)?$")


def _parse_version(version: str) -> tuple[tuple[int, int, int], list[str]]:
    match = _VERSION_RE.match(str(version or ""))
    if not match:
        return (0, 0, 0), []
    core = tuple(int(part or 0) for part in match.group(1, 2, 3))
    pre = match.group(4)
    return core, (pre.split(".") if pre else [])  # type: ignore[return-value]


def _compare_identifier(a: str, b: str) -> int:
    a_num, b_num = a.isdigit(), b.isdigit()
    if a_num and b_num:
        return (int(a) > int(b)) - (int(a) < int(b))
    if a_num != b_num:
        return -1 if a_num else 1
    return (a > b) - (a < b)


def compare_versions(a: str, b: str) -> int:
    """Semver order with prereleases below their release.

    Must match firmware/newhorizons_os/VersionCompare.cpp -- both are checked
    against spec/fixtures/versions.json.
    """
    a_core, a_pre = _parse_version(a)
    b_core, b_pre = _parse_version(b)
    if a_core != b_core:
        return 1 if a_core > b_core else -1
    if not a_pre or not b_pre:
        return (not a_pre) - (not b_pre)
    for left, right in zip(a_pre, b_pre):
        result = _compare_identifier(left, right)
        if result:
            return result
    return (len(a_pre) > len(b_pre)) - (len(a_pre) < len(b_pre))


def is_prerelease(version: str) -> bool:
    return bool(_parse_version(version)[1])


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


def _read_latest(path: Path) -> str:
    try:
        return str(json.loads(path.read_text(encoding="utf-8")).get("latest") or "")
    except (OSError, ValueError):
        return ""


def publish_track(
    *,
    manifest_dir: Path,
    track: str,
    firmware_path: Path,
    model: str,
    version: str,
    base_url: str,
    changelog_url: str = "",
    channel: str = "stable",
    stable_aliases: tuple[str, ...] = (),
) -> list[Path]:
    """Writes one release into a board's manifests, by channel.

    stable -> <track>-<version>.json, <track>-latest.json (plus any legacy
              aliases), and <track>-beta.json too unless the beta channel
              already holds something newer: beta devices get every release.
    beta   -> <track>-<version>.json and <track>-beta.json only.

    <track>-latest.json is what firmware older than the channels reads, so a
    prerelease must never reach it: that would push the beta onto every
    device in the field, and auto_apply_on_boot is on by default.
    """
    if channel not in CHANNELS:
        raise ValueError(f"unknown channel {channel!r}; expected one of {CHANNELS}")
    manifest_dir = Path(manifest_dir)
    prerelease = is_prerelease(version)
    if channel == "stable" and prerelease:
        raise ValueError(f"{version} is a prerelease; publish it with --channel beta")
    if channel == "beta" and not prerelease:
        raise ValueError(f"{version} has no prerelease suffix; beta versions look like v1.14.0-beta.1")
    latest_path = manifest_dir / f"{track}-latest.json"
    beta_path = manifest_dir / f"{track}-beta.json"
    if channel == "beta":
        stable = _read_latest(latest_path)
        if stable and compare_versions(version, stable) <= 0:
            raise ValueError(f"beta {version} is not newer than stable {stable}")

    targets = [manifest_dir / f"{track}-{version}.json"]
    if channel == "stable":
        targets.append(latest_path)
        targets.extend(manifest_dir / alias for alias in stable_aliases)
        current_beta = _read_latest(beta_path)
        if not current_beta or compare_versions(version, current_beta) >= 0:
            targets.append(beta_path)
    else:
        targets.append(beta_path)

    for target in targets:
        write_manifest(
            output_path=target,
            firmware_path=firmware_path,
            model=model,
            version=version,
            base_url=base_url,
            changelog_url=changelog_url,
        )
    return targets


def main() -> None:
    parser = argparse.ArgumentParser(description="Generate New Horizons Arduino OTA manifest JSON.")
    parser.add_argument("--firmware", required=True, type=Path)
    target = parser.add_mutually_exclusive_group(required=True)
    target.add_argument("--output", type=Path, help="write a single manifest file")
    target.add_argument("--track", help="board manifest prefix, e.g. arduino-v15f: writes every file the channel needs")
    parser.add_argument("--manifest-dir", type=Path, help="where --track manifests live")
    parser.add_argument("--channel", choices=CHANNELS, default="stable")
    parser.add_argument("--stable-alias", action="append", default=[], help="extra file name kept equal to -latest.json")
    # Pre-rename board name on purpose -- see NHOS_BOARD_OTA_MODEL in BoardConfig.h.
    parser.add_argument("--model", default="VD-CTL/R v1.0.F 2026.4")
    parser.add_argument("--version", required=True)
    parser.add_argument("--base-url", required=True)
    parser.add_argument("--changelog-url", default="")
    args = parser.parse_args()
    if args.track:
        if args.manifest_dir is None:
            parser.error("--track needs --manifest-dir")
        try:
            written = publish_track(
                manifest_dir=args.manifest_dir,
                track=args.track,
                firmware_path=args.firmware,
                model=args.model,
                version=args.version,
                base_url=args.base_url,
                changelog_url=args.changelog_url,
                channel=args.channel,
                stable_aliases=tuple(args.stable_alias),
            )
        except ValueError as exc:
            parser.error(str(exc))
        for path in written:
            print(f"manifest: {path}", file=sys.stderr)
        return
    if args.channel == "stable" and is_prerelease(args.version) and args.output.name.endswith("-latest.json"):
        parser.error(f"{args.version} is a prerelease and must not be written to {args.output.name}")
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
