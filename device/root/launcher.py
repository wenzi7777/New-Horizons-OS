import machine
import sys
import time

if "recovery" not in sys.path:
    sys.path.insert(0, "recovery")

import immutable_config as iconfig

try:
    import uos as os
except ImportError:
    import os


_CONFIG_DEFAULTS = {
    "DEFAULT_MODE": "recovery",
    "OS_DIR": "nhos",
    "DEVICE_STATE_DIR": "device_state",
    "LOG_PATH": "data/logs/device.log",
}

BOOT_LOGS = []


def _ensure_config_defaults():
    for name, value in _CONFIG_DEFAULTS.items():
        if not hasattr(iconfig, name):
            setattr(iconfig, name, value)


_ensure_config_defaults()


class BootLogger:
    def _emit(self, level, message):
        BOOT_LOGS.append((level.lower(), message))
        try:
            stamp = time.ticks_ms()
        except AttributeError:
            stamp = int(time.time() * 1000)
        print("{} [{}] {}".format(stamp, level, message))

    def info(self, message):
        self._emit("INFO", message)

    def warn(self, message):
        self._emit("WARN", message)

    def error(self, message):
        self._emit("ERROR", message)


def _runtime_path():
    return iconfig.DEVICE_STATE_DIR + "/runtime_config.json"


def _load_json(path, default):
    try:
        import ujson as json
    except ImportError:
        import json
    try:
        with open(path, "r") as handle:
            return json.load(handle)
    except (OSError, ValueError):
        return default


def _write_json(path, data):
    try:
        import ujson as json
    except ImportError:
        import json
    tmp_path = path + ".tmp"
    with open(tmp_path, "w") as handle:
        json.dump(data, handle)
    try:
        os.remove(path)
    except OSError:
        pass
    os.rename(tmp_path, path)


def _load_runtime():
    data = _load_json(_runtime_path(), {})
    if isinstance(data, dict):
        return data
    return {}


def _update_runtime(updates):
    runtime = _load_runtime()
    runtime.update(updates)
    _write_json(_runtime_path(), runtime)
    return runtime


def _exists(path):
    try:
        os.stat(path)
        return True
    except OSError:
        return False


def _collect():
    try:
        import gc
        gc.collect()
    except Exception:
        pass


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


def _schedule_recovery_reboot(logger, error):
    logger.error("launcher_os_failed {}".format(error))
    try:
        _update_runtime({"mode": "recovery", "boot_request": "recovery"})
    except Exception as exc:
        logger.error("launcher_recovery_schedule_failed {}".format(exc))
    time.sleep_ms(250)
    machine.reset()


def run():
    logger = BootLogger()
    wifi_setup_requested = _wait_boot_window(logger)

    try:
        runtime = _load_runtime()
        mode = runtime.get("mode", iconfig.DEFAULT_MODE)
        boot_request = runtime.get("boot_request", "")
        os_installed = _exists(iconfig.OS_DIR + "/app.mpy")
        logger.info("launcher_mode={} os_installed={} boot_request={}".format(mode, os_installed, boot_request))

        if boot_request == "recovery" or mode == "recovery":
            _run_recovery(wifi_setup_requested=wifi_setup_requested)
        elif os_installed:
            sys.path.insert(0, iconfig.OS_DIR)
            try:
                _collect()
                from app import App
                try:
                    App(wifi_setup_requested=wifi_setup_requested).run()
                except Exception as exc:
                    _schedule_recovery_reboot(logger, str(exc))
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
