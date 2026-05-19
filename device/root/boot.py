import sys

if "recovery" not in sys.path:
    sys.path.insert(0, "recovery")

import immutable_config as iconfig
import storage


_CONFIG_DEFAULTS = {
    "DEVICE_STATE_DIR": "device_state",
    "RECOVERY_DIR": "recovery",
    "OS_DIR": "nhos",
    "OTA_STAGE_DIR": "ota_stage",
    "CALIBRATION_DIR": "device_state/calibration",
    "DATA_FILES_DIR": "data/files",
    "DATA_LOG_DIR": "data/logs",
    "DATA_TMP_DIR": "data/tmp",
}


for name, value in _CONFIG_DEFAULTS.items():
    if not hasattr(iconfig, name):
        setattr(iconfig, name, value)


for path in (
    iconfig.DEVICE_STATE_DIR,
    iconfig.RECOVERY_DIR,
    iconfig.OS_DIR,
    iconfig.OTA_STAGE_DIR,
    iconfig.CALIBRATION_DIR,
    iconfig.DATA_FILES_DIR,
    iconfig.DATA_LOG_DIR,
    iconfig.DATA_TMP_DIR,
):
    storage.ensure_dir(path)
