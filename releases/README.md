# Releases

Arduino OTA releases publish whole firmware images and JSON manifests.

Generated binaries should come from:

```bash
VERSION=vX.Y.Z firmware/scripts/build_arduino_release.sh
VERSION=vX.Y.Z firmware/scripts/build_arduino_release_gcu_lts.sh
VERSION=vX.Y.Z firmware/scripts/build_arduino_release_gcu_v22c_lts.sh
VERSION=vX.Y.Z firmware/scripts/build_arduino_release_gcu_v21_lts.sh
```

Generated binaries are copied to `releases/artifacts/` so tagged raw GitHub URLs can serve OTA payloads.

Publish two manifest tracks:

- `releases/arduino-v10f-latest.json` and `releases/arduino-v10f-vX.Y.Z.json` for `VD-CTL/R v1.0.F 2026.4`
- `releases/arduino-gcu-v23d-lts-latest.json` and `releases/arduino-gcu-v23d-lts-vX.Y.Z.json` for `VD-CTL/R v2.3.D GCU LTS`
- `releases/arduino-gcu-v22c-lts-latest.json` and `releases/arduino-gcu-v22c-lts-vX.Y.Z.json` for `VD-CTL/R v2.2.C GCU LTS`
- `releases/arduino-gcu-v21-lts-latest.json` and `releases/arduino-gcu-v21-lts-vX.Y.Z.json` for `VD-CTL/R v2.1 GCU LTS`

Do not publish the old MicroPython TLV per-file manifests from this repository anymore.
