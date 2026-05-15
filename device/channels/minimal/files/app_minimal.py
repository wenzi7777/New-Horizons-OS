import machine
import time

import immutable_config as iconfig

from device_identity import get_device_id, get_device_name, get_device_uid
from device_logging import DeviceLogger
from filesystem_api import FilesystemAPI
from runtime_config import RuntimeConfigStore
from udp_control import UDPControlServer
from update_manager import UpdateManager
from wifi_manager import WiFiManager


class MinimalApp:
    def __init__(self, wifi_setup_requested=False, recovery_error=""):
        self.wifi_setup_requested = wifi_setup_requested
        self.recovery_error = recovery_error
        self.device_id = get_device_id()
        self.device_uid = get_device_uid()
        self.device_name = get_device_name(iconfig.FIRMWARE_NAME)
        self.reboot_required = False
        self.logger = DeviceLogger(iconfig.LOG_PATH)
        self.config_store = RuntimeConfigStore(iconfig.DEVICE_STATE_DIR)
        self.runtime = self.config_store.load_runtime()
        self.filesystem = FilesystemAPI(iconfig.DEVICE_STATE_DIR)
        self.wifi = WiFiManager(self.config_store, self.logger)
        self.control = UDPControlServer(iconfig.DEFAULT_CONTROL_PORT, self.logger)
        self.updates = UpdateManager(self.config_store, self.logger, ".")
        self.last_connect_ms = 0
        self.boot_network_initialized = False
        self.last_status_announce_ms = 0
        self.reboot_deadline_ms = None

    def setup(self):
        wifi_ok = False
        if self.wifi_setup_requested or not self._has_network_hint():
            self.wifi.start_setup_portal("boot_window" if self.wifi_setup_requested else "missing_credentials")
        else:
            wifi_ok = self.wifi.connect()
            if not wifi_ok:
                self.wifi.start_setup_portal("connect_failed")
        self.control.begin()
        if wifi_ok and self.runtime.get("update", {}).get("check_on_boot", True):
            result = self.updates.check()
            self.logger.info("boot_update_check={}".format(result.get("status")))
        if wifi_ok:
            self._announce_status(force=True)
        self.boot_network_initialized = wifi_ok

    def run(self):
        self.setup()
        while True:
            now = time.ticks_ms()
            self.wifi.service_setup_portal()
            self.control.poll(self._handle_request)
            update_result = self.updates.service()
            if update_result is not None:
                self._handle_update_result(update_result, now)
                self._announce_status(now, force=True)

            if self.wifi.is_connected() and not self.boot_network_initialized:
                self.boot_network_initialized = True
                if self.runtime.get("update", {}).get("check_on_boot", True):
                    result = self.updates.check()
                    self.logger.info("portal_update_check={}".format(result.get("status")))
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

    def _announce_status(self, now=None, force=False):
        if self.control.sock is None or not self.wifi.is_connected():
            return False
        if now is None:
            now = time.ticks_ms()
        if (not force
                and time.ticks_diff(now, self.last_status_announce_ms) < iconfig.STATUS_ANNOUNCE_INTERVAL_MS):
            return False
        master = self.runtime.get("master_server", {})
        host = master.get("host", "")
        if not host:
            return False
        port = int(master.get("port", iconfig.DEFAULT_CONTROL_PORT))
        payload = {
            "status": "ok",
            "message": "status_announce",
            "device_id": "0x{:08X}".format(self.device_id),
            "device_uid": self.device_uid,
            "device_name": self.device_name,
            "channel": self.runtime.get("channel", "minimal"),
            "manifest_url": self.runtime.get("update", {}).get("manifest_url", ""),
            "runtime": self.runtime,
            "wifi_connected": self.wifi.is_connected(),
            "wifi_setup": self.wifi.portal_status(),
            "update_state": self.updates.status(),
            "recovery_error": self.recovery_error,
            "reboot_required": self.reboot_required,
        }
        ok = self.control.send(host, port, payload)
        if ok:
            self.last_status_announce_ms = now
        return ok

    def _reboot_due(self, now):
        if self.reboot_deadline_ms is None:
            return True
        return time.ticks_diff(now, self.reboot_deadline_ms) >= 0

    def _handle_update_result(self, result, now):
        if result.get("status") == "ok" and result.get("reboot_required"):
            self.reboot_required = True
            if self.reboot_deadline_ms is None:
                self.reboot_deadline_ms = time.ticks_add(now, 1200)

    def _handle_request(self, request, addr):
        command = request.get("command", "status")

        if command in ("status", "query"):
            runtime = self.config_store.load_runtime()
            return {
                "status": "ok",
                "message": "minimal_status",
                "device_id": "0x{:08X}".format(self.device_id),
                "device_uid": self.device_uid,
                "device_name": self.device_name,
                "channel": runtime.get("channel", "minimal"),
                "manifest_url": runtime.get("update", {}).get("manifest_url", ""),
                "runtime": runtime,
                "wifi_connected": self.wifi.is_connected(),
                "wifi_setup": self.wifi.portal_status(),
                "update_state": self.updates.status(),
                "recovery_error": self.recovery_error,
                "reboot_required": self.reboot_required,
            }

        if command == "check_update":
            return self.updates.start_check()

        if command == "apply_update":
            return self.updates.start_apply()

        if command == "read_logs":
            max_lines = int(request.get("max_lines", 50))
            return {"status": "ok", "message": "logs", "lines": self.logger.read_tail(max_lines), "reboot_required": False}

        if command == "fs_list":
            return {"status": "ok", "message": "fs_list", "items": self.filesystem.list_files(), "reboot_required": False}

        if command == "fs_read":
            path = request.get("path", "")
            data = self.filesystem.read_file(path)
            if data is None:
                return {"status": "error", "message": "missing_file", "error": path, "reboot_required": False}
            return {"status": "ok", "message": "fs_read", "file": data, "reboot_required": False}

        if command == "set_channel":
            channel = request.get("channel", "minimal")
            manifest_url = request.get("manifest_url", iconfig.DEFAULT_MANIFESTS.get(channel, iconfig.DEFAULT_MANIFESTS["minimal"]))
            runtime = self.config_store.update_runtime({
                "channel": channel,
                "update": {"manifest_url": manifest_url, "enabled": True},
            })
            self.runtime = runtime
            return {"status": "ok", "message": "channel_updated", "runtime": runtime, "reboot_required": False}

        if command == "set_servers":
            runtime = self.config_store.update_runtime({
                "master_server": request.get("master_server", {}),
                "data_server": request.get("data_server", {}),
            })
            self.runtime = runtime
            return {"status": "ok", "message": "servers_updated", "runtime": runtime, "reboot_required": False}

        if command == "upgrade_to_full":
            if self.updates.is_busy():
                state = self.updates.status()
                return {
                    "status": "error",
                    "message": "update_busy",
                    "error": state.get("phase", "busy"),
                    "reboot_required": state.get("reboot_required", False),
                    "update_state": state,
                }
            runtime = self.config_store.update_runtime({
                "channel": "full",
                "update": {"manifest_url": iconfig.DEFAULT_MANIFESTS["full"], "enabled": True},
            })
            self.runtime = runtime
            result = self.updates.start_apply()
            result["runtime"] = runtime
            result["message"] = "upgrade_to_full_started" if result.get("status") == "ok" else result.get("message", "")
            return result

        if command == "start_wifi_setup":
            self.wifi.start_setup_portal("udp_command")
            return {"status": "ok", "message": "wifi_setup_started", "wifi_setup": self.wifi.portal_status(), "reboot_required": False}

        if command == "stop_wifi_setup":
            self.wifi.stop_setup_portal()
            return {"status": "ok", "message": "wifi_setup_stopped", "wifi_setup": self.wifi.portal_status(), "reboot_required": False}

        if command == "set_wifi":
            result = self.wifi.apply_credentials(request.get("ssid", ""), request.get("password", ""))
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


def run(wifi_setup_requested=False, recovery_error=""):
    MinimalApp(wifi_setup_requested=wifi_setup_requested, recovery_error=recovery_error).run()
