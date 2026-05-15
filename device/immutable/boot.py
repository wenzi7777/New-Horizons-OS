import immutable_config as iconfig
import storage


for path in (
    iconfig.DEVICE_STATE_DIR,
    iconfig.CALIBRATION_DIR,
    iconfig.DEVICE_STATE_DIR + "/logs",
):
    storage.ensure_dir(path)
