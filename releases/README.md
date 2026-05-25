# Releases

Arduino OTA releases publish whole firmware images and JSON manifests.

Generated binaries should come from:

```bash
VERSION=vX.Y.Z firmware/scripts/build_arduino_release.sh
```

Generated binaries are copied to `releases/artifacts/` so tagged raw GitHub URLs can serve OTA payloads. Generated manifests should be written here as `arduino-latest.json` or versioned JSON files.

Do not publish the old MicroPython TLV per-file manifests from this repository anymore.
