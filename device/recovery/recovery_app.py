import machine
import time
import gc
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
from wifi_manager import WiFiManager

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
        self.filesystem = FilesystemAPI(
            getattr(iconfig, "DATA_FILES_DIR", "data/files"),
            getattr(iconfig, "DATA_TMP_DIR", "data/tmp"),
            {
                "user": getattr(iconfig, "DATA_FILES_DIR", "data/files"),
                "logs": getattr(iconfig, "DATA_LOG_DIR", "data/logs"),
                "calibration": getattr(iconfig, "CALIBRATION_DIR", "device_state/calibration"),
            },
        )
        self.wifi = WiFiManager(self.config_store, self.logger)
        self.mqtt_transport = MQTTTransport(lambda: self.runtime, self.device_uid, self.logger)
        self.os_writer = None
        self.update_state = self._default_update_state()
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
                self._os_installed(),
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
        if wifi_ok:
            self._announce_status(force=True)
        self.boot_network_initialized = wifi_ok

    def run(self):
        self.setup()
        while True:
            now = time.ticks_ms()
            self._service_wifi_setup_portal()
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

    def _os_installed(self):
        os_dir = getattr(iconfig, "OS_DIR", "nhos")
        return self._path_exists(os_dir + "/app.mpy")

    def _service_wifi_setup_portal(self):
        handled = self.wifi.service_setup_portal()
        if handled:
            self._reload_runtime_from_store("wifi_portal")
        return handled

    def _reload_runtime_from_store(self, source="runtime"):
        runtime = self.config_store.load_runtime()
        if runtime == self.runtime:
            return False
        old_transport = self.runtime.get("transport", {})
        old_mqtt = self.runtime.get("mqtt", {})
        self.runtime = runtime
        self._apply_logging_config(self.runtime.get("logging", {}))
        if old_transport != self.runtime.get("transport", {}) or old_mqtt != self.runtime.get("mqtt", {}):
            if hasattr(self.mqtt_transport, "reconfigure"):
                self.mqtt_transport.reconfigure()
            else:
                self.mqtt_transport.close()
        if self.logger is not None:
            self.logger.info("runtime_config_reloaded source={}".format(source))
        return True

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
            "system": self._system_status(self.runtime),
            "mode": "recovery",
            "manifest_url": self.runtime.get("update", {}).get("manifest_url", ""),
            "runtime": self.runtime,
            "wifi_connected": self.wifi.is_connected(),
            "wifi_setup": self.wifi.portal_status(),
            "update_state": self._current_update_state(),
            "recovery_error": self.recovery_error,
            "reboot_required": self.reboot_required,
        }
        ok = self.mqtt_transport.publish_status(payload, self.wifi.is_connected())
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
                "system": self._system_status(runtime),
                "memory": self._memory_status(),
                "mode": "recovery",
                "manifest_url": runtime.get("update", {}).get("manifest_url", ""),
                "runtime": runtime,
                "logging": self._logging_status(),
                "wifi_connected": self.wifi.is_connected(),
                "wifi_setup": self.wifi.portal_status(),
                "update_state": self._current_update_state(),
                "recovery_error": self.recovery_error,
                "reboot_required": self.reboot_required,
            }

        if command == "check_os_release":
            self._prepare_ota_memory()
            release_url = self._release_url(request)
            writer = self._ensure_os_writer()
            result = writer.check_os_release(release_url)
            result["release_url"] = release_url
            self._set_update_state({
                "phase": "ready",
                "operation": "check_os_release",
                "version": result.get("latest_version", ""),
                "manifest_url": result.get("manifest_url", ""),
                "total_files": 0,
                "applied_files": 0,
                "downloaded_files": 0,
                "skipped_files": 0,
                "current_file": "",
                "last_error": "",
                "last_result": "manifest_ready",
                "reboot_required": False,
            })
            result["update_state"] = self._current_update_state()
            return result

        if command == "write_os":
            self._prepare_ota_memory()
            release_url = self._release_url(request)
            writer = self._ensure_os_writer()
            result = writer.write_os(release_url)
            result["release_url"] = release_url
            installed_version = result.get("version", "")
            if installed_version:
                self.runtime = self.config_store.update_runtime({"system": {"os_version": installed_version}})
            self._set_update_state({
                "phase": "done",
                "operation": "write_os",
                "version": result.get("version", ""),
                "total_files": int(result.get("downloaded_files", 0)) + int(result.get("skipped_files", 0)),
                "applied_files": int(result.get("downloaded_files", 0)) + int(result.get("skipped_files", 0)),
                "downloaded_files": int(result.get("downloaded_files", 0)),
                "skipped_files": int(result.get("skipped_files", 0)),
                "current_file": "",
                "last_error": "",
                "last_result": "applied",
                "reboot_required": bool(result.get("reboot_required", False)),
            })
            result["update_state"] = self._current_update_state()
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

        if command in ("file_list", "fs_list"):
            scope = request.get("scope", "user")
            return {
                "status": "ok",
                "message": command,
                "scope": scope,
                "items": self.filesystem.list_files(scope),
                "storage": self.filesystem.usage(),
                "reboot_required": False,
            }

        if command == "fs_read":
            path = request.get("path", "")
            data = self.filesystem.read_file(path, request.get("scope", "user"))
            if data is None:
                return {"status": "error", "message": "missing_file", "error": path, "reboot_required": False}
            return {"status": "ok", "message": "fs_read", "file": data, "reboot_required": False}

        if command == "file_upload_begin":
            return self._file_call(
                self.filesystem.upload_begin,
                request.get("path", ""),
                request.get("size", 0),
                request.get("sha256", ""),
                request.get("scope", "user"),
            )

        if command == "file_upload_chunk":
            return self._file_call(
                self.filesystem.upload_chunk,
                request.get("path", ""),
                request.get("offset", 0),
                request.get("data", ""),
                request.get("scope", "user"),
            )

        if command == "file_upload_finish":
            return self._file_call(self.filesystem.upload_finish, request.get("path", ""), request.get("scope", "user"))

        if command == "file_download_begin":
            return self._file_call(self.filesystem.download_begin, request.get("path", ""), request.get("scope", "user"))

        if command == "file_download_chunk":
            return self._file_call(
                self.filesystem.download_chunk,
                request.get("path", ""),
                request.get("offset", 0),
                request.get("length", 1024),
                request.get("scope", "user"),
            )

        if command == "file_delete":
            try:
                deleted = self.filesystem.delete_file(request.get("path", ""), request.get("scope", "user"))
            except ValueError as exc:
                return {"status": "error", "message": str(exc), "error": str(exc), "reboot_required": False}
            return {
                "status": "ok",
                "message": "file_deleted" if deleted else "file_not_found",
                "applied": bool(deleted),
                "reboot_required": False,
            }

        if command == "set_servers":
            runtime_patch = {
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
                    "mode": "mqtt",
                    "topic_namespace": request.get("topic_namespace", iconfig.DEFAULT_TOPIC_NAMESPACE),
                }
            })
            self.runtime = runtime
            return {"status": "ok", "message": "transport_updated", "runtime": runtime, "reboot_required": False}

        if command == "start_wifi_setup":
            self.wifi.start_setup_portal("mqtt_command")
            return {"status": "ok", "message": "wifi_setup_started", "wifi_setup": self.wifi.portal_status(), "reboot_required": False}

        if command == "stop_wifi_setup":
            self.wifi.stop_setup_portal()
            return {"status": "ok", "message": "wifi_setup_stopped", "wifi_setup": self.wifi.portal_status(), "reboot_required": False}

        if command == "set_wifi":
            result = self.wifi.apply_credentials(
                request.get("ssid", ""),
                request.get("password", ""),
                request.get("server_profile", ""),
                request.get("mqtt_host", ""),
                request.get("mqtt_port", ""),
                request.get("mqtt_tls", ""),
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

    def _file_call(self, fn, *args):
        try:
            return fn(*args)
        except ValueError as exc:
            return {"status": "error", "message": str(exc), "error": str(exc), "reboot_required": False}

    def _installed_os_version(self, runtime=None):
        runtime = runtime if isinstance(runtime, dict) else getattr(self, "runtime", {})
        system = runtime.get("system", {}) if isinstance(runtime, dict) else {}
        version = system.get("os_version", "")
        if version:
            return version
        state = self._current_update_state()
        version = state.get("version", "")
        if version:
            return version
        return "unknown" if self._os_installed() else ""

    def _system_status(self, runtime=None):
        return {
            "name": self.device_name,
            "hardware_model": getattr(iconfig, "HARDWARE_MODEL", "unknown"),
            "runtime_version": getattr(iconfig, "RUNTIME_VERSION", "unknown"),
            "mode": "recovery",
            "os_installed": self._os_installed(),
            "os_version": self._installed_os_version(runtime),
            "recovery_version": getattr(iconfig, "RECOVERY_VERSION", getattr(iconfig, "FIRMWARE_VERSION", "unknown")),
        }

    def _memory_status(self):
        try:
            free = int(gc.mem_free())
        except Exception:
            free = 0
        try:
            allocated = int(gc.mem_alloc())
        except Exception:
            allocated = 0
        total = free + allocated if free or allocated else 0
        return {
            "heap_free": free,
            "heap_allocated": allocated,
            "heap_total": total,
            "heap_used_percent": int((allocated * 100) // total) if total else 0,
        }

    def _release_url(self, request):
        return getattr(iconfig, "DEFAULT_RELEASE_URL", "")

    def _ensure_os_writer(self):
        if self.os_writer is None:
            global OSWriter
            if OSWriter is None:
                from os_writer import OSWriter as LoadedOSWriter
                OSWriter = LoadedOSWriter
            self.os_writer = OSWriter(".", self.logger, progress=self._os_write_progress)
        return self.os_writer

    def _prepare_ota_memory(self):
        try:
            if hasattr(self.wifi, "release_setup_portal"):
                self.wifi.release_setup_portal()
            else:
                self.wifi.stop_setup_portal()
        except Exception as exc:
            if self.logger is not None:
                self.logger.warn("ota_prepare_wifi_release_failed {}".format(exc))
        try:
            gc.collect()
        except Exception:
            pass

    def _default_update_state(self):
        return {
            "phase": "idle",
            "operation": "",
            "version": "",
            "manifest_url": "",
            "total_files": 0,
            "applied_files": 0,
            "downloaded_files": 0,
            "skipped_files": 0,
            "current_file": "",
            "last_error": "",
            "last_result": "",
            "reboot_required": False,
        }

    def _current_update_state(self):
        state = getattr(self, "update_state", None)
        if not isinstance(state, dict):
            state = self._default_update_state()
            self.update_state = state
        return state

    def _set_update_state(self, patch):
        state = self._default_update_state()
        state.update(self._current_update_state())
        state.update(patch or {})
        self.update_state = state
        return state

    def _os_write_progress(self, payload):
        raw_phase = str(payload.get("phase", "") or "")
        downloaded = int(payload.get("written_files", payload.get("downloaded_files", 0)) or 0)
        skipped = int(payload.get("skipped_files", 0) or 0)
        total = int(payload.get("total_files", downloaded + skipped) or 0)
        phase = "done" if raw_phase == "complete" else "downloading"
        last_result = "applied" if raw_phase == "complete" else raw_phase
        self._set_update_state({
            "phase": phase,
            "operation": "write_os",
            "version": payload.get("version", ""),
            "total_files": total,
            "applied_files": min(total, downloaded + skipped) if total else downloaded + skipped,
            "downloaded_files": downloaded,
            "skipped_files": skipped,
            "current_file": "" if raw_phase == "complete" else payload.get("current_file", ""),
            "last_error": "",
            "last_result": last_result,
            "reboot_required": raw_phase == "complete",
        })
        self._announce_status(force=True)

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
