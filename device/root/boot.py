import sys

if "recovery" not in sys.path:
    sys.path.insert(0, "recovery")

import immutable_config as iconfig
import storage


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
