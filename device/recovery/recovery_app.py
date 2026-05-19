import machine
import time
try:
    import uos as os
except ImportError:  # pragma: no cover - CPython fallback
    import os

import immutable_config as iconfig

from device_identity import get_device_id, get_device_name, get_device_uid
from device_logging import DeviceLogger
from filesystem_api import FilesystemAPI
from mqtt_transport import MQTTTransport
from runtime_config import RuntimeConfigStore
from udp_control import UDPControlServer
from wifi_manager import WiFiManager
try:
    from os_writer import OSWriter
except ImportError:
    OSWriter = None


class RecoveryApp:
    def __init__(self, wifi_setup_requested=False, recovery_error=""):
        self.wifi_setup_requested = wifi_setup_requested
        self.recovery_error = recovery_error
        self.device_id = get_device_id()
        self.device_uid = get_device_uid()
        self.device_name = get_device_name(iconfig.FIRMWARE_NAME)
        self.reboot_required = False
        self.config_store = RuntimeConfigStore(iconfig.DEVICE_STATE_DIR)
        self.runtime = self.config_store.load_runtime()
        self.logger = DeviceLogger(iconfig.LOG_PATH)
        self._apply_logging_config(self.runtime.get("logging", {}))
        self.filesystem = FilesystemAPI(iconfig.DEVICE_STATE_DIR)
        self.wifi = WiFiManager(self.config_store, self.logger)
        self.control = UDPControlServer(iconfig.DEFAULT_CONTROL_PORT, self.logger)
        self.mqtt_transport = MQTTTransport(lambda: self.runtime, self.device_uid, self.logger)
        self.os_writer = None
        self.last_connect_ms = 0
        self.boot_network_initialized = False
        self.last_status_announce_ms = 0
        self.reboot_deadline_ms = None

    def setup(self):
        self.logger.info(
            "boot mode=recovery device={} id={} version={} os_installed={} wifi_setup={}".format(
                self.device_name,
                hex(self.device_id),
                getattr(iconfig, "FIRMWARE_VERSION", "unknown"),
                self._path_exists(getattr(iconfig, "OS_DIR", "nhos") + "/main.py"),
                bool(self.wifi_setup_requested),
            )
        )
        wifi_ok = False
        if self.wifi_setup_requested or not self._has_network_hint():
            self.wifi.start_setup_portal("boot_window" if self.wifi_setup_requested else "missing_credentials")
        else:
            wifi_ok = self.wifi.connect()
            if not wifi_ok:
                self.wifi.start_setup_portal("connect_failed")
        self.control.begin()
        self.logger.info("control_server_started port={}".format(iconfig.DEFAULT_CONTROL_PORT))
        if wifi_ok:
            self._announce_status(force=True)
        self.boot_network_initialized = wifi_ok

    def run(self):
        self.setup()
        while True:
            now = time.ticks_ms()
            self.wifi.service_setup_portal()
            self.control.poll(self._handle_request)
            self.mqtt_transport.poll(self.wifi.is_connected(), self._handle_request)

            if self.wifi.is_connected() and not self.boot_network_initialized:
                self.boot_network_initialized = True
                self._announce_status(now, force=True)

            if (not self.wifi.is_connected()
                    and not self.wifi.setup_active()
                    and self._has_network_hint()
                    and time.ticks_diff(now, self.last_connect_ms) >= 10000):
                self.last_connect_ms = now
                if not self.wifi.connect():
                    self.wifi.start_setup_portal("reconnect_failed")

            self._announce_status(now)
            if self.reboot_required and self._reboot_due(now):
                time.sleep_ms(250)
                machine.reset()
            time.sleep_ms(50)

    def _has_network_hint(self):
        network_cfg = self.config_store.load_network()
        return bool(network_cfg.get("ssid"))

    def _path_exists(self, path):
        try:
            os.stat(path)
            return True
        except OSError:
            return False

    def _announce_status(self, now=None, force=False):
        if not self.wifi.is_connected():
            return False
        if now is None:
            now = time.ticks_ms()
        if (not force
                and time.ticks_diff(now, self.last_status_announce_ms) < iconfig.STATUS_ANNOUNCE_INTERVAL_MS):
            return False
        payload = {
            "status": "ok",
            "message": "status_announce",
            "device_id": "0x{:08X}".format(self.device_id),
            "device_uid": self.device_uid,
            "device_name": self.device_name,
            "mode": "recovery",
            "manifest_url": self.runtime.get("update", {}).get("manifest_url", ""),
            "runtime": self.runtime,
            "wifi_connected": self.wifi.is_connected(),
            "wifi_setup": self.wifi.portal_status(),
            "update_state": {},
            "recovery_error": self.recovery_error,
            "reboot_required": self.reboot_required,
        }
        if self._transport_mode() == "mqtt":
            ok = self.mqtt_transport.publish_status(payload, self.wifi.is_connected())
        else:
            if self.control.sock is None:
                return False
            master = self.runtime.get("master_server", {})
            host = master.get("host", "")
            if not host:
                return False
            port = int(master.get("port", iconfig.DEFAULT_CONTROL_PORT))
            ok = self.control.send(host, port, payload)
        if ok:
            self.last_status_announce_ms = now
        return ok

    def _reboot_due(self, now):
        if self.reboot_deadline_ms is None:
            return True
        return time.ticks_diff(now, self.reboot_deadline_ms) >= 0

    def _handle_request(self, request, addr):
        command = request.get("command", "status")

        if command in ("status", "query"):
            runtime = self.config_store.load_runtime()
            return {
                "status": "ok",
                "message": "recovery_status",
                "device_id": "0x{:08X}".format(self.device_id),
                "device_uid": self.device_uid,
                "device_name": self.device_name,
                "mode": "recovery",
                "manifest_url": runtime.get("update", {}).get("manifest_url", ""),
                "runtime": runtime,
                "logging": self._logging_status(),
                "wifi_connected": self.wifi.is_connected(),
                "wifi_setup": self.wifi.portal_status(),
                "update_state": {},
                "recovery_error": self.recovery_error,
                "reboot_required": self.reboot_required,
            }

        if command == "check_os_release":
            release_url = self._release_url(request)
            writer = self._ensure_os_writer()
            result = writer.check_os_release(release_url)
            result["release_url"] = release_url
            return result

        if command == "write_os":
            release_url = self._release_url(request)
            writer = self._ensure_os_writer()
            result = writer.write_os(release_url)
            result["release_url"] = release_url
            return result

        if command == "reboot_to_os":
            runtime = self.config_store.update_runtime({"mode": "normal", "boot_request": ""})
            self.runtime = runtime
            self.reboot_required = True
            self.reboot_deadline_ms = None
            return {"status": "ok", "message": "reboot_to_os_scheduled", "reboot_required": True}

        if command == "read_logs":
            max_lines = int(request.get("max_lines", 50))
            lines = self.logger.read_tail(max_lines) if self.logger is not None else []
            return {"status": "ok", "message": "logs", "lines": lines, "reboot_required": False}

        if command in ("log_clear", "clear_logs"):
            if self.logger is not None and hasattr(self.logger, "clear"):
                self.logger.clear()
            return {"status": "ok", "message": "log_cleared", "applied": True, "reboot_required": False}

        if command == "set_logging":
            return self._set_logging(request)

        if command == "fs_list":
            return {"status": "ok", "message": "fs_list", "items": self.filesystem.list_files(), "reboot_required": False}

        if command == "fs_read":
            path = request.get("path", "")
            data = self.filesystem.read_file(path)
            if data is None:
                return {"status": "error", "message": "missing_file", "error": path, "reboot_required": False}
            return {"status": "ok", "message": "fs_read", "file": data, "reboot_required": False}

        if command == "set_servers":
            runtime_patch = {
                "master_server": request.get("master_server", {}),
                "data_server": request.get("data_server", {}),
                "mqtt": request.get("mqtt", {}),
            }
            if request.get("server_profile", ""):
                runtime_patch["server_profile"] = request.get("server_profile", "")
            runtime = self.config_store.update_runtime(runtime_patch)
            self.runtime = runtime
            return {"status": "ok", "message": "servers_updated", "runtime": runtime, "reboot_required": False}

        if command == "set_transport":
            runtime = self.config_store.update_runtime({
                "transport": {
                    "mode": request.get("mode", "mqtt"),
                    "topic_namespace": request.get("topic_namespace", iconfig.DEFAULT_TOPIC_NAMESPACE),
                }
            })
            self.runtime = runtime
            return {"status": "ok", "message": "transport_updated", "runtime": runtime, "reboot_required": False}

        if command == "start_wifi_setup":
            self.wifi.start_setup_portal("udp_command")
            return {"status": "ok", "message": "wifi_setup_started", "wifi_setup": self.wifi.portal_status(), "reboot_required": False}

        if command == "stop_wifi_setup":
            self.wifi.stop_setup_portal()
            return {"status": "ok", "message": "wifi_setup_stopped", "wifi_setup": self.wifi.portal_status(), "reboot_required": False}

        if command == "set_wifi":
            result = self.wifi.apply_credentials(
                request.get("ssid", ""),
                request.get("password", ""),
                request.get("server_profile", ""),
                request.get("master_host", ""),
                request.get("master_port", ""),
                request.get("data_host", ""),
                request.get("data_port", ""),
                request.get("mqtt_host", ""),
                request.get("mqtt_port", ""),
                request.get("mqtt_tls", ""),
                request.get("transport_mode", ""),
                request.get("release_url", ""),
                request.get("log_enabled", ""),
                request.get("log_capacity", ""),
            )
            self.runtime = self.config_store.load_runtime()
            self._apply_logging_config(self.runtime.get("logging", {}))
            return {
                "status": "ok" if result.get("ok") else "error",
                "message": result.get("message", ""),
                "wifi_setup": self.wifi.portal_status(),
                "reboot_required": False,
                "error": "" if result.get("ok") else "wifi_connect_failed",
            }

        if command == "reset_credentials":
            self.wifi.clear_credentials()
            self.wifi.start_setup_portal("credentials_reset")
            return {"status": "ok", "message": "credentials_reset", "wifi_setup": self.wifi.portal_status(), "reboot_required": False}

        if command == "reboot":
            self.reboot_required = True
            self.reboot_deadline_ms = None
            return {"status": "ok", "message": "rebooting", "reboot_required": True}

        return {"status": "error", "message": "unknown_command", "error": command, "reboot_required": False}

    def _transport_mode(self):
        runtime = getattr(self, "runtime", None) or {}
        if not isinstance(runtime, dict):
            return "udp"
        return runtime.get("transport", {}).get("mode", "udp")

    def _release_url(self, request):
        runtime = self.config_store.load_runtime()
        update_cfg = runtime.get("update", {})
        return (
            request.get("release_url", "")
            or update_cfg.get("release_url", "")
            or getattr(iconfig, "DEFAULT_RELEASE_URL", "")
        )

    def _ensure_os_writer(self):
        if self.os_writer is None:
            global OSWriter
            if OSWriter is None:
                from os_writer import OSWriter as LoadedOSWriter
                OSWriter = LoadedOSWriter
            self.os_writer = OSWriter(".", self.logger)
        return self.os_writer

    def _set_logging(self, request):
        logging_cfg = self._normalize_logging_config(request)
        self.runtime = self.config_store.update_runtime({"logging": logging_cfg})
        self._apply_logging_config(logging_cfg)
        return {
            "status": "ok",
            "message": "logging_updated",
            "applied": True,
            "reboot_required": False,
            "logging": self._logging_status(),
        }

    def _normalize_logging_config(self, source):
        capacity = str(source.get("capacity", "default") or "default")
        if capacity not in ("default", "extended"):
            capacity = "default"
        return {
            "enabled": self._normalize_logging_enabled(source.get("enabled", True)),
            "capacity": capacity,
            "serial": "status",
        }

    def _normalize_logging_enabled(self, value):
        normalized = str(value).strip().lower()
        if normalized in ("0", "false", "no", "off", "disabled"):
            return False
        if normalized in ("1", "true", "yes", "on", "enabled"):
            return True
        return bool(value)

    def _apply_logging_config(self, logging_cfg):
        if self.logger is None or not hasattr(self.logger, "configure"):
            return
        logging_cfg = self._normalize_logging_config(logging_cfg or {})
        self.logger.configure(
            enabled=logging_cfg.get("enabled", True),
            capacity=logging_cfg.get("capacity", "default"),
        )

    def _logging_status(self):
        if self.logger is not None and hasattr(self.logger, "settings"):
            return self.logger.settings()
        return self._normalize_logging_config(self.runtime.get("logging", {}))


def run(wifi_setup_requested=False, recovery_error=""):
    RecoveryApp(wifi_setup_requested=wifi_setup_requested, recovery_error=recovery_error).run()
