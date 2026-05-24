# app.py
#
# Keep this module small: launcher imports it before normal OS Wi-Fi setup.
# The full application lives in app_core.py and is loaded after a lightweight
# Wi-Fi preconnect attempt has given lwIP/DHCP more heap headroom.
import gc
import sys
import time


_BOOT_SHADOW_MODULES = (
    "config",
    "device_identity",
    "device_logging",
    "findme",
    "fs_core",
    "nhcp",
    "runtime_config",
    "secrets",
    "storage",
    "udp_control",
    "wifi_manager",
)


def _purge_boot_shadow_modules():
    for name in _BOOT_SHADOW_MODULES:
        try:
            sys.modules.pop(name, None)
        except Exception:
            pass


class _PreconnectLogger:
    def _emit(self, level, message):
        try:
            stamp = time.ticks_ms()
        except Exception:
            stamp = 0
        print("{} [{}] {}".format(stamp, level, message))

    def info(self, message):
        self._emit("INFO", message)

    def warn(self, message):
        self._emit("WARN", message)

    def error(self, message):
        self._emit("ERROR", message)


def _preconnect_wifi(wifi_setup_requested=False):
    if wifi_setup_requested:
        return False
    try:
        import config
        from runtime_config import RuntimeConfigStore
        from wifi_manager import WiFiManager

        store = RuntimeConfigStore(config.DEVICE_STATE_DIR)
        network_cfg = store.load_network()
        if not network_cfg.get("ssid"):
            return False
        wifi = WiFiManager(store, _PreconnectLogger())
        ok = wifi.connect()
        gc.collect()
        return bool(ok)
    except Exception as exc:
        _PreconnectLogger().warn("os_preconnect_failed {}".format(exc))
        return False


class App:
    def __init__(self, wifi_setup_requested=False):
        self.wifi_setup_requested = wifi_setup_requested

    def run(self):
        _purge_boot_shadow_modules()
        _preconnect_wifi(self.wifi_setup_requested)
        from app_core import App as CoreApp
        return CoreApp(wifi_setup_requested=self.wifi_setup_requested).run()
