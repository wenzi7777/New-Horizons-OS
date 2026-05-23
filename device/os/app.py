# app.py
#
# Keep this module small: launcher imports it before normal OS Wi-Fi setup.
# The full application lives in app_core.py and is loaded after a lightweight
# Wi-Fi preconnect attempt has given lwIP/DHCP more heap headroom.
import gc
import time


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
        object.__setattr__(self, "wifi_setup_requested", wifi_setup_requested)
        object.__setattr__(self, "_core", None)

    def _load_core(self):
        core = object.__getattribute__(self, "_core")
        if core is None:
            from app_core import App as CoreApp
            core = CoreApp(wifi_setup_requested=object.__getattribute__(self, "wifi_setup_requested"))
            object.__setattr__(self, "_core", core)
        return core

    def run(self):
        _preconnect_wifi(object.__getattribute__(self, "wifi_setup_requested"))
        return self._load_core().run()

    def __getattr__(self, name):
        return getattr(self._load_core(), name)

    def __setattr__(self, name, value):
        if name in ("wifi_setup_requested", "_core"):
            object.__setattr__(self, name, value)
        else:
            setattr(self._load_core(), name, value)
