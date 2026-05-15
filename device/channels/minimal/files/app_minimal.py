import machine
import time

import immutable_config as iconfig
import vdboard

from device_logging import DeviceLogger
from filesystem_api import FilesystemAPI
from runtime_config import RuntimeConfigStore
from udp_control import UDPControlServer
from update_manager import UpdateManager
from wifi_manager import WiFiManager


class MinimalApp:
    def __init__(self, provisioning_requested=False, recovery_error=""):
        self.provisioning_requested = provisioning_requested
        self.recovery_error = recovery_error
        self.reboot_required = False
        self.logger = DeviceLogger(iconfig.LOG_PATH)
        self.config_store = RuntimeConfigStore(iconfig.DEVICE_STATE_DIR)
        self.runtime = self.config_store.load_runtime()
        self.filesystem = FilesystemAPI(iconfig.DEVICE_STATE_DIR)
        self.wifi = WiFiManager(self.config_store, self.logger)
        self.control = UDPControlServer(iconfig.DEFAULT_CONTROL_PORT, self.logger)
        self.updates = UpdateManager(self.config_store, self.logger, ".")
        self.last_connect_ms = 0

    def setup(self):
        if self.provisioning_requested:
            vdboard.prov.start_ble(iconfig.DEFAULT_POP, iconfig.DEFAULT_SERVICE_NAME)
        elif self._has_network_hint():
            self.wifi.connect()
        self.control.begin()
        if self.wifi.is_connected() and self.runtime.get("update", {}).get("check_on_boot", True):
            result = self.updates.check()
            self.logger.info("boot_update_check={}".format(result.get("status")))

    def run(self):
        self.setup()
        while True:
            now = time.ticks_ms()
            self.control.poll(self._handle_request)

            if (not self.wifi.is_connected()
                    and vdboard.prov.status() not in ("starting", "waiting_credentials", "connecting_wifi")
                    and self._has_network_hint()
                    and time.ticks_diff(now, self.last_connect_ms) >= 10000):
                self.last_connect_ms = now
                self.wifi.connect()

            if self.reboot_required:
                time.sleep_ms(250)
                machine.reset()
            time.sleep_ms(50)

    def _has_network_hint(self):
        network_cfg = self.config_store.load_network()
        return bool(network_cfg.get("ssid") or vdboard.prov.is_provisioned())

    def _handle_request(self, request, addr):
        command = request.get("command", "status")

        if command in ("status", "query"):
            return {
                "status": "ok",
                "message": "minimal_status",
                "channel": self.config_store.load_runtime().get("channel", "minimal"),
                "manifest_url": self.config_store.load_runtime().get("update", {}).get("manifest_url", ""),
                "wifi_connected": self.wifi.is_connected(),
                "provisioning": vdboard.prov.status(),
                "provisioned": vdboard.prov.is_provisioned(),
                "recovery_error": self.recovery_error,
                "reboot_required": False,
            }

        if command == "check_update":
            return self.updates.check()

        if command == "apply_update":
            result = self.updates.apply()
            if result.get("reboot_required"):
                self.reboot_required = True
            return result

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
            return {"status": "ok", "message": "channel_updated", "runtime": runtime, "reboot_required": False}

        if command == "reboot":
            self.reboot_required = True
            return {"status": "ok", "message": "rebooting", "reboot_required": True}

        return {"status": "error", "message": "unknown_command", "error": command, "reboot_required": False}


def run(provisioning_requested=False, recovery_error=""):
    MinimalApp(provisioning_requested=provisioning_requested, recovery_error=recovery_error).run()
