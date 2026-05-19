import machine
import sys
import time

if "recovery" not in sys.path:
    sys.path.insert(0, "recovery")

import immutable_config as iconfig
from device_logging import DeviceLogger
from runtime_config import RuntimeConfigStore
import storage


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


def run():
    logger = DeviceLogger(iconfig.LOG_PATH)
    wifi_setup_requested = _wait_boot_window(logger)
    store = RuntimeConfigStore(iconfig.DEVICE_STATE_DIR)
    runtime = store.load_runtime()
    channel = runtime.get("channel", iconfig.DEFAULT_CHANNEL)
    boot_request = runtime.get("boot_request", "")
    os_installed = storage.exists("os/app.py") or storage.exists("os/main.py")
    legacy_full_installed = storage.exists("app.py")
    logger.info("launcher_channel={} os_installed={} boot_request={}".format(channel, os_installed, boot_request))

    try:
        if boot_request == "recovery" or channel == "minimal":
            import recovery_app
            recovery_app.run(wifi_setup_requested=wifi_setup_requested)
        elif os_installed:
            sys.path.insert(0, "os")
            try:
                from app import App
                App(wifi_setup_requested=wifi_setup_requested).run()
            finally:
                try:
                    sys.path.pop(0)
                except Exception:
                    pass
        elif channel == "full" and legacy_full_installed:
            from app import App
            App(wifi_setup_requested=wifi_setup_requested).run()
        else:
            import recovery_app
            recovery_app.run(wifi_setup_requested=wifi_setup_requested)
    except Exception as exc:
        logger.error("launcher_fallback {}".format(exc))
        import recovery
        recovery.run(wifi_setup_requested=wifi_setup_requested, error=str(exc))
