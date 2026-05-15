import machine
import time

import immutable_config as iconfig
from device_logging import DeviceLogger
from runtime_config import RuntimeConfigStore


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
    logger.info("launcher_channel={}".format(channel))

    try:
        if channel == "full":
            from app import App
            App(wifi_setup_requested=wifi_setup_requested).run()
        else:
            import app_minimal
            app_minimal.run(wifi_setup_requested=wifi_setup_requested)
    except Exception as exc:
        logger.error("launcher_fallback {}".format(exc))
        import recovery
        recovery.run(wifi_setup_requested=wifi_setup_requested, error=str(exc))
