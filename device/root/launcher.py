import machine
import sys
import time

if "recovery" not in sys.path:
    sys.path.insert(0, "recovery")

import immutable_config as iconfig
from device_logging import DeviceLogger
from runtime_config import RuntimeConfigStore
import storage


_CONFIG_DEFAULTS = {
    "DEFAULT_MODE": "recovery",
    "OS_DIR": "nhos",
    "DEVICE_STATE_DIR": "device_state",
    "LOG_PATH": "data/logs/device.log",
}


def _ensure_config_defaults():
    for name, value in _CONFIG_DEFAULTS.items():
        if not hasattr(iconfig, name):
            setattr(iconfig, name, value)


_ensure_config_defaults()


def _wait_boot_window(logger):
    button = machine.Pin(iconfig.ACTION_BUTTON_PIN, machine.Pin.IN, machine.Pin.PULL_UP)
    started = time.ticks_ms()
    logger.info("boot_window_start")
    while time.ticks_diff(time.ticks_ms(), started) < iconfig.BOOT_WINDOW_MS:
        if button.value() == 0:
            logger.warn("wifi_setup_requested")
            return True
        time.sleep_ms(iconfig.BOOT_WINDOW_POLL_MS)
    return False


def _run_recovery(wifi_setup_requested=False, error=""):
    import recovery_app
    recovery_app.run(wifi_setup_requested=wifi_setup_requested, recovery_error=error)


def _schedule_recovery_reboot(store, logger, error):
    logger.error("launcher_os_failed {}".format(error))
    try:
        store.update_runtime({"mode": "recovery", "boot_request": "recovery"})
    except Exception as exc:
        logger.error("launcher_recovery_schedule_failed {}".format(exc))
    time.sleep_ms(250)
    machine.reset()


def run():
    logger = DeviceLogger(iconfig.LOG_PATH)
    wifi_setup_requested = _wait_boot_window(logger)

    try:
        store = RuntimeConfigStore(iconfig.DEVICE_STATE_DIR)
        runtime = store.load_runtime()
        mode = runtime.get("mode", iconfig.DEFAULT_MODE)
        boot_request = runtime.get("boot_request", "")
        os_installed = storage.exists(iconfig.OS_DIR + "/app.py") or storage.exists(iconfig.OS_DIR + "/main.py")
        logger.info("launcher_mode={} os_installed={} boot_request={}".format(mode, os_installed, boot_request))

        if boot_request == "recovery" or mode == "recovery":
            _run_recovery(wifi_setup_requested=wifi_setup_requested)
        elif os_installed:
            sys.path.insert(0, iconfig.OS_DIR)
            try:
                from app import App
                try:
                    App(wifi_setup_requested=wifi_setup_requested).run()
                except Exception as exc:
                    _schedule_recovery_reboot(store, logger, str(exc))
                    return
            finally:
                try:
                    sys.path.pop(0)
                except Exception:
                    pass
        else:
            _run_recovery(wifi_setup_requested=wifi_setup_requested)
    except Exception as exc:
        logger.error("launcher_fallback {}".format(exc))
        _run_recovery(wifi_setup_requested=wifi_setup_requested, error=str(exc))
