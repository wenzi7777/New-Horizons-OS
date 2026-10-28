# nhos_core

The code the device firmware (`firmware/newhorizons_os`) and the New Horizons
Hub (`New-Horizons-Hub/newhorizons_hub`) share. One copy, used by both: it
replaces the files the Hub used to keep byte-identical copies of.

Build either sketch with `--library firmware/libraries/nhos_core` (the build,
flash and release scripts, CI and `scripts/check_all.sh` all do). The Hub
points at this directory in a NewHorizonsOS-OTA checkout beside it.

Rules for what lives here:

- **Self-contained.** Nothing here includes a product header (`Config.h`,
  `Storage.h`, ...). Files that need one stay in each product, as their own
  versions (see the Hub README for the list).
- **Board facts only.** `BoardConfig.h` / `BoardPins.*` describe the hardware;
  product policy stays in each product's `Config.h`. Every build must define
  exactly one `NHOS_BOARD_*`.
- **Generated files** (`PacketWireConstants.h`, `TimeWireConstants.h`) come
  from `spec/wire.json` via `firmware/scripts/gen_wire.py`; do not edit them.
  `SetupPortalAsset.h` comes from `portal/setup_portal.html` via
  `firmware/scripts/gen_portal_asset.py` (the SoftAP setup page, gzipped).
- Plain C++ where possible, so `tests_native/` can compile it on a desktop.
