# app.py
import machine
import time
import gc

import config
import board_pins

from device_identity import get_device_id, get_device_name, get_device_uid
from device_logging import DeviceLogger
from runtime_config import RuntimeConfigStore
from wifi_manager import WiFiManager


class App:
    def __init__(self, wifi_setup_requested=False):
        self.wifi_setup_requested = wifi_setup_requested
        self.frame_id = 0
        self.device_id = get_device_id()
        self.device_uid = get_device_uid()
        self.device_name = get_device_name(config.DEVICE_NAME)
        self.reboot_required = False
        self.mode = "normal"
        self.maintenance_reason = ""
        self.last_connect_ms = 0
        self.boot_network_initialized = False
        self.hardware_ready = False
        self.scan_ready = False

        self.config_store = RuntimeConfigStore(config.DEVICE_STATE_DIR)
        self.runtime = self.config_store.load_runtime()
        self.logger = DeviceLogger(config.LOG_PATH)
        self._apply_logging_config(self.runtime.get("logging", {}))
        self.network = self.config_store.load_network()
        self.filter_config = self.config_store.load_filter()

        self.wifi = WiFiManager(self.config_store, self.logger)
        self.filesystem = None
        self.time_sync = None
        self.calibration = None
        self.tx_buffer = None
        self.packet = None
        self.filter_chain = None
        self.scan_rate = None
        self.vdboard = None
        self.decode_scan_frame = None
        self.native_streaming = False

        self.imu = None
        self.battery = None
        self.led = None
        self.mqtt_transport = None
        self.recovery_writer = None
        self.update_state = self._default_update_state()

        self.latest_matrix = None
        self.latest_frame = None
        self.latest_imu = None
        self.latest_battery = None

        self.last_imu_ms = 0
        self.last_battery_ms = 0
        self.last_led_ms = 0
        self.last_ntp_retry_ms = 0
        self.last_status_announce_ms = 0
        self.reboot_deadline_ms = None
        self.send_backoff_until_ms = 0
        self.last_gc_frame_id = 0

        self.sent_packets = 0
        self.failed_sends = 0

    def setup(self):
        self.logger.info("setup_start")
        self._print_setup()

        if config.PRINT_PIN_CONFLICTS:
            conflicts = board_pins.validate_pins()
            if conflicts:
                for pin, names in conflicts.items():
                    self.logger.warn("pin_conflict gpio={} roles={}".format(pin, names))

        if self.wifi_setup_requested or not self._has_network_hint():
            reason = "boot_window" if self.wifi_setup_requested else "missing_credentials"
            self.wifi.start_setup_portal(reason)
            self.logger.info("setup_wifi_portal_started reason={}".format(reason))
            wifi_ok = False
        else:
            self.logger.info("setup_wifi_connect_start")
            wifi_ok = self.wifi.connect()
            self.logger.info("setup_wifi_connect_done ok={}".format(bool(wifi_ok)))
            if not wifi_ok:
                self.logger.warn("setup_wifi_connect_failed_offline")

        self._ensure_led()
        if self.led:
            self.logger.info("setup_led_begin_start")
            self.led.begin()
            self.logger.info("setup_led_begin_done")
            self.logger.info("setup_led_boot_window_start")
            self.led.set_boot_window()
            self.logger.info("setup_led_boot_window_done")
            if self.wifi.setup_active():
                self.led.set_wifi_setup()

        if wifi_ok:
            self._ensure_streaming_services()

        if wifi_ok:
            self.logger.info("setup_ntp_sync_start")
            self._sync_time()
            self.logger.info("setup_ntp_sync_done")
            self.logger.info("setup_status_announce_start")
            self._announce_status(force=True)
            self.logger.info("setup_status_announce_done")
            self._ensure_runtime_hardware()
        self.boot_network_initialized = wifi_ok

        self.update_led_state()
        self.logger.info("setup_done")

    def run(self):
        self.setup()

        target_fps = self.runtime.get("scan_timing", {}).get("target_fps", config.TARGET_FPS)
        scan_interval = int(1000 / max(1, int(target_fps)))
        imu_interval = int(1000 / max(1, int(config.IMU_RATE_HZ)))
        battery_interval = int(1000 / max(1, int(config.BATTERY_RATE_HZ)))
        led_interval = int(1000 / max(1, int(config.LED_RATE_HZ)))

        while True:
            now = time.ticks_ms()
            self.wifi.service_setup_portal()
            if self.mqtt_transport is not None:
                self.mqtt_transport.poll(self.wifi.is_connected(), self._handle_control_request)
            if self.wifi.is_connected() and not self.boot_network_initialized:
                self.boot_network_initialized = True
                self._ensure_streaming_services()
                self._sync_time()
                self._announce_status(now, force=True)
                self.update_led_state()

            if (self.boot_network_initialized
                    and not self.hardware_ready
                    and not self.wifi.setup_active()
                    and not self._in_maintenance()):
                self._ensure_runtime_hardware()
                self.update_led_state()

            if (not self._in_maintenance()
                    and self.hardware_ready
                    and self.imu
                    and time.ticks_diff(now, self.last_imu_ms) >= imu_interval):
                self.last_imu_ms = now
                self.latest_imu = self.imu.read()
                self._update_native_imu_cache()

            if (not self._in_maintenance()
                    and self.hardware_ready
                    and self.battery
                    and time.ticks_diff(now, self.last_battery_ms) >= battery_interval):
                self.last_battery_ms = now
                self._service_battery_status()
                self._update_native_battery_cache()

            if self.scan_ready and not self._in_maintenance():
                self.vdboard.scan.service()
                self.handle_scan(now)

            if self._in_maintenance():
                self._clear_tx_buffer()
            else:
                self.handle_transmit()

            if (not self.wifi.is_connected()
                    and not self.wifi.setup_active()
                    and self._has_network_hint()
                    and time.ticks_diff(now, self.last_connect_ms) >= 10000):
                self.last_connect_ms = now
                wifi_ok = self.wifi.connect()
                if wifi_ok:
                    self.update_led_state()
                else:
                    self.logger.warn("wifi_reconnect_failed_offline")
                    self.update_led_state()

            if self.led and time.ticks_diff(now, self.last_led_ms) >= led_interval:
                self.last_led_ms = now
                self.led.update()

            if (self.time_sync is not None
                    and not self.time_sync.status()["synced"]
                    and time.ticks_diff(now, self.last_ntp_retry_ms) >= 60000):
                self.last_ntp_retry_ms = now
                if self.wifi.is_connected():
                    self._sync_time()

            self._announce_status(now)

            if self.reboot_required and self._reboot_due(now):
                self.logger.warn("reboot_required_restart")
                time.sleep_ms(250)
                machine.reset()

            self._maybe_collect_garbage()

    def handle_scan(self, timestamp_ms):
        if getattr(self, "native_streaming", False) and hasattr(self.vdboard.scan, "pop_packet"):
            packet = self.vdboard.scan.pop_packet()
            if packet is None:
                return

            stats = self.vdboard.scan.stats()
            if len(stats) >= 5:
                self.frame_id = int(stats[4])
            else:
                self.frame_id += 1
            fps = self.scan_rate.tick() if self.scan_rate is not None else None
            if config.PRINT_FPS and fps is not None:
                self.logger.info(
                    "scan fps={} wifi={} sent={} fail={}".format(
                        fps,
                        self.wifi.is_connected(),
                        self.sent_packets,
                        self.failed_sends
                    )
                )

            send_every = self.runtime.get("scan_timing", {}).get(
                "send_every_n_frames",
                config.SEND_EVERY_N_FRAMES
            )
            if send_every > 1 and (self.frame_id % send_every != 0):
                return
            if self._transmit_backing_off(timestamp_ms):
                return
            self.send_packet(packet)
            return

        frame_view = self.vdboard.scan.pop_frame_mv()
        if frame_view is None:
            return
        frame_info = self.decode_scan_frame(bytes(frame_view))
        raw_matrix = frame_info["payload_mv"]
        self.latest_frame = frame_info
        self.latest_matrix = self._apply_sensor_pipeline(raw_matrix)

        send_every = self.runtime.get("scan_timing", {}).get(
            "send_every_n_frames",
            config.SEND_EVERY_N_FRAMES
        )
        self.frame_id = int(frame_info["seq"])
        fps = self.scan_rate.tick()
        if config.PRINT_FPS and fps is not None:
            self.logger.info(
                "scan fps={} wifi={} sent={} fail={}".format(
                    fps,
                    self.wifi.is_connected(),
                    self.sent_packets,
                    self.failed_sends
                )
            )
        if send_every > 1 and (self.frame_id % send_every != 0):
            return
        if self._transmit_backing_off(timestamp_ms):
            return

        packet = self.packet.build(
            frame_id=self.frame_id,
            timestamp_ms=int(frame_info["timestamp_ms"] if self.time_sync.status()["synced"] else timestamp_ms),
            matrix=self.latest_matrix,
            imu=self.latest_imu,
            battery=self.latest_battery
        )

        if config.USE_PACKET_BUFFER and self.tx_buffer is not None:
            self.tx_buffer.push(packet)
        else:
            self.send_packet(packet)

    def handle_transmit(self):
        if self._in_maintenance():
            self._clear_tx_buffer()
            return
        if not config.USE_PACKET_BUFFER or self.tx_buffer is None:
            return

        if not self.wifi.is_connected():
            self.tx_buffer.clear()
            return
        if self.mqtt_transport is None:
            self.tx_buffer.clear()
            return
        if self._transmit_backing_off():
            self.tx_buffer.clear()
            return

        max_send = config.SEND_MAX_PER_LOOP
        for _ in range(max_send):
            packet = self.tx_buffer.pop()
            if packet is None:
                return
            if not self.send_packet(packet):
                self.tx_buffer.clear()
                return

    def send_packet(self, packet):
        if self._in_maintenance():
            return False
        if self.mqtt_transport is None:
            return False

        ok = self.mqtt_transport.publish_raw(packet, self.wifi.is_connected())
        if ok:
            self.sent_packets += 1
            self.send_backoff_until_ms = 0
            return True

        self.failed_sends += 1
        self.send_backoff_until_ms = time.ticks_add(
            time.ticks_ms(),
            getattr(config, "SEND_FAILURE_BACKOFF_MS", 100)
        )
        return False

    def _transmit_backing_off(self, now=None):
        if not self.send_backoff_until_ms:
            return False
        if now is None:
            now = time.ticks_ms()
        return time.ticks_diff(now, self.send_backoff_until_ms) < 0

    def _maybe_collect_garbage(self):
        interval = int(getattr(config, "GC_EVERY_N_FRAMES", 0))
        if interval <= 0 or self.frame_id <= 0:
            return False
        if self.frame_id - self.last_gc_frame_id < interval:
            return False
        self.last_gc_frame_id = self.frame_id
        gc.collect()
        return True

    def update_led_state(self):
        if not self.led:
            return
        if self.reboot_required:
            if hasattr(self.led, "set_reboot_required"):
                self.led.set_reboot_required()
            else:
                self.led.set_updating()
            return
        if self._in_maintenance() and hasattr(self.led, "set_maintenance"):
            self.led.set_maintenance()
            return
        if self.wifi.setup_active():
            self.led.set_wifi_setup()
            return
        if self.battery:
            if self.latest_battery is None:
                self.latest_battery = self.battery.read_status()
            if self.battery.is_charging():
                self.led.set_charging()
                return
            if self.battery.is_charge_done():
                self.led.set_charge_done()
                return
        if self.wifi.is_connected():
            self.led.set_normal()
        else:
            self.led.set_error()

    def _service_battery_status(self):
        if not self.battery:
            return False
        previous_status = getattr(self.battery, "last_status_code", None)
        self.latest_battery = self.battery.read_status()
        current_status = getattr(self.battery, "last_status_code", None)
        changed = current_status != previous_status
        if changed:
            self.update_led_state()
        return changed

    def _apply_sensor_pipeline(self, matrix):
        processed = []
        active_rows = self._active_rows()
        active_cols_list = self._active_cols()
        active_cols = len(active_cols_list)
        for idx, value in enumerate(matrix):
            filtered = self.filter_chain.process(idx, value)
            row_index = idx // active_cols
            col_index = idx % active_cols
            analog_pin = active_rows[row_index]
            select_pin = active_cols_list[col_index]
            calibrated = self.calibration.apply(analog_pin, select_pin, filtered)
            processed.append(float(calibrated))
        return processed

    def _sync_time(self):
        if self.time_sync is None:
            return
        synced = self.time_sync.sync()
        if synced:
            self.logger.info("ntp_sync_ok epoch={}".format(self.time_sync.now_epoch()))
        else:
            self.logger.warn("ntp_sync_failed error={}".format(self.time_sync.last_error))

    def _reboot_due(self, now):
        if self.reboot_deadline_ms is None:
            return True
        return time.ticks_diff(now, self.reboot_deadline_ms) >= 0

    def _announce_status(self, now=None, force=False):
        if self.mqtt_transport is None:
            return False
        if not self.wifi.is_connected():
            return False
        if now is None:
            now = time.ticks_ms()
        if (not force
                and time.ticks_diff(now, self.last_status_announce_ms) < config.STATUS_ANNOUNCE_INTERVAL_MS):
            return False
        payload = self._status()
        payload["message"] = "status_announce"
        ok = self.mqtt_transport.publish_status(payload, self.wifi.is_connected())
        if ok:
            self.last_status_announce_ms = now
        return ok

    def _apply_runtime_reload(self):
        self.runtime = self.config_store.load_runtime()
        self._apply_logging_config(self.runtime.get("logging", {}))
        self.filter_config = self.config_store.load_filter()
        self._apply_matrix_layout()
        self.time_sync.servers = list(self.runtime.get("ntp_servers", []))

    def _authorized(self, addr):
        return bool(addr and addr[0] == "mqtt")

    def _handle_control_request(self, request, addr):
        self._ensure_streaming_services()
        if not self._authorized(addr):
            self.logger.warn("control_unauthorized from={}:{}".format(addr[0], addr[1]))
            return None

        cmd = request.get("command", request.get("cmd", "")).strip().lower()
        if not cmd:
            return self._ok("missing_command", error="missing_command")

        if cmd in ("check_os_release", "write_os", "reboot_to_os"):
            return {
                "status": "error",
                "message": "requires_recovery",
                "next_command": "reboot_to_recovery",
                "reboot_required": False,
                "applied": False,
            }

        if cmd in ("status", "query"):
            return self._status()

        if cmd == "maintenance_status":
            return self._maintenance_status()

        if cmd == "enter_maintenance":
            return self._enter_maintenance(request.get("reason", ""))

        if cmd == "exit_maintenance":
            return self._exit_maintenance()

        if cmd == "reboot_to_recovery":
            self.config_store.update_runtime({"mode": "recovery", "boot_request": "recovery"})
            self.reboot_required = True
            return self._ok("reboot_to_recovery_scheduled", applied=True, reboot_required=True)

        if self._in_maintenance() and cmd not in self._maintenance_allowed_commands():
            return self._ok("maintenance_command_disabled", error="maintenance_command_disabled")

        if cmd in self._maintenance_service_commands():
            if not self._in_maintenance():
                return self._ok("maintenance_required", error="maintenance_required")
            self._ensure_maintenance_services()

        if cmd == "check_recovery_release":
            return self._check_recovery_release(request)

        if cmd == "write_recovery":
            return self._write_recovery(request)

        if cmd == "reload_config":
            self._apply_runtime_reload()
            return self._ok("config_reloaded", applied=True)

        if cmd == "set_servers":
            runtime_patch = {
                "mqtt": request.get("mqtt", {}),
            }
            if request.get("server_profile", ""):
                runtime_patch["server_profile"] = request.get("server_profile", "")
            runtime = self.config_store.update_runtime(runtime_patch)
            self.runtime = runtime
            return self._ok("servers_updated", applied=True)

        if cmd == "set_transport":
            runtime = self.config_store.update_runtime({
                "transport": {
                    "mode": "mqtt",
                    "topic_namespace": request.get("topic_namespace", "newhorizons/v1"),
                },
            })
            self.runtime = runtime
            return self._ok("transport_updated", applied=True)

        if cmd == "set_logging":
            return self._set_logging(request)

        if cmd == "set_filter":
            filter_data = self.config_store.update_filter(request.get("filter", {}))
            self.filter_config = filter_data
            self._configure_native_filter()
            if self.filter_chain is not None:
                self.filter_chain.apply_config(
                    filter_data.get("enabled", False),
                    filter_data.get("median", 3),
                    filter_data.get("alpha", 0.25),
                )
            return self._ok("filter_updated", applied=True)

        if cmd == "set_matrix_layout":
            try:
                rows, cols = self._validate_matrix_layout(
                    request.get("analog_pins", []),
                    request.get("select_pins", []),
                )
            except ValueError as exc:
                return self._ok("matrix_layout_invalid", error=str(exc))
            runtime = self.config_store.update_runtime({
                "matrix_layout": {
                    "active_rows": rows,
                    "active_cols": cols,
                },
            })
            self.runtime = runtime
            self._apply_matrix_layout()
            return {
                "status": "ok",
                "message": "matrix_layout_updated",
                "runtime": runtime,
                "reboot_required": False,
                "applied": True,
            }

        if cmd == "calibration_sample_cell":
            return self._maintenance_sample_cell(request)

        if cmd == "calibration_sample_all":
            return self._maintenance_sample_all(request)

        if cmd == "calibration_save":
            if self.calibration is not None:
                self.calibration.save()
            self._reload_native_calibration()
            return self._ok("calibration_saved", applied=True)

        if cmd == "dump_calibration":
            level = request.get("level")
            if level is not None:
                return {
                    "status": "ok",
                    "message": "calibration_level_dump",
                    "reboot_required": False,
                    "applied": False,
                    "data": self.calibration.dump_level(level, self._active_rows(), self._active_cols()),
                }
            return {
                "status": "ok",
                "message": "calibration_dump",
                "reboot_required": False,
                "applied": False,
                "data": self.calibration.dump(),
                "levels": self.calibration.list_levels(),
            }

        if cmd == "delete_calibration_level":
            self.calibration.delete_level(request["level"])
            self.calibration.save()
            self._reload_native_calibration()
            return self._ok("calibration_level_deleted", applied=True)

        if cmd == "reboot":
            self.reboot_required = True
            return self._ok("reboot_scheduled", applied=True, reboot_required=True)

        if cmd == "reset_credentials":
            self.wifi.clear_credentials()
            self.wifi.start_setup_portal("credentials_reset")
            self.update_led_state()
            return self._ok("credentials_reset", applied=True)

        if cmd == "start_wifi_setup":
            self.wifi.start_setup_portal("mqtt_command")
            self.update_led_state()
            return {
                "status": "ok",
                "message": "wifi_setup_started",
                "reboot_required": False,
                "applied": True,
                "wifi_setup": self.wifi.portal_status(),
                "error": "",
            }

        if cmd == "stop_wifi_setup":
            self.wifi.stop_setup_portal()
            self.update_led_state()
            return {
                "status": "ok",
                "message": "wifi_setup_stopped",
                "reboot_required": False,
                "applied": True,
                "wifi_setup": self.wifi.portal_status(),
                "error": "",
            }

        if cmd == "set_wifi":
            result = self.wifi.apply_credentials(
                request.get("ssid", ""),
                request.get("password", ""),
                request.get("server_profile", ""),
                request.get("mqtt_host", ""),
                request.get("mqtt_port", ""),
                request.get("mqtt_tls", ""),
                "",
                request.get("log_enabled", ""),
                request.get("log_capacity", ""),
            )
            self.runtime = self.config_store.load_runtime()
            self._apply_logging_config(self.runtime.get("logging", {}))
            self.update_led_state()
            return {
                "status": "ok" if result.get("ok") else "error",
                "message": result.get("message", ""),
                "reboot_required": False,
                "applied": bool(result.get("ok")),
                "wifi_setup": self.wifi.portal_status(),
                "error": "" if result.get("ok") else "wifi_connect_failed",
            }

        if cmd == "log_read_tail":
            return self._log_tail(request)

        if cmd == "log_tail":
            return self._log_tail(request)

        if cmd == "log_clear":
            if hasattr(self.logger, "clear"):
                self.logger.clear()
            else:
                import storage
                storage.remove(config.LOG_PATH)
                storage.remove(config.LOG_PATH + ".1")
            return self._ok("log_cleared", applied=True)

        if cmd == "file_list":
            return {
                "status": "ok",
                "message": "file_list",
                "reboot_required": False,
                "applied": False,
                "scope": request.get("scope", "user"),
                "items": self.filesystem.list_files(request.get("scope", "user")),
                "storage": self.filesystem.usage(),
            }

        if cmd == "file_upload_begin":
            return self._file_call(
                self.filesystem.upload_begin,
                request.get("path", ""),
                request.get("size", 0),
                request.get("sha256", ""),
                request.get("scope", "user"),
            )

        if cmd == "file_upload_chunk":
            return self._file_call(
                self.filesystem.upload_chunk,
                request.get("path", ""),
                request.get("offset", 0),
                request.get("data", ""),
                request.get("scope", "user"),
            )

        if cmd == "file_upload_finish":
            return self._file_call(self.filesystem.upload_finish, request.get("path", ""), request.get("scope", "user"))

        if cmd == "file_download_begin":
            return self._file_call(self.filesystem.download_begin, request.get("path", ""), request.get("scope", "user"))

        if cmd == "file_download_chunk":
            return self._file_call(
                self.filesystem.download_chunk,
                request.get("path", ""),
                request.get("offset", 0),
                request.get("length", 1024),
                request.get("scope", "user"),
            )

        if cmd == "file_delete":
            try:
                deleted = self.filesystem.delete_file(request.get("path", ""), request.get("scope", "user"))
            except ValueError as exc:
                return self._ok(str(exc), error=str(exc))
            return self._ok("file_deleted" if deleted else "file_not_found", applied=bool(deleted))

        if cmd == "fs_list":
            return {
                "status": "ok",
                "message": "fs_list",
                "reboot_required": False,
                "applied": False,
                "scope": request.get("scope", "user"),
                "items": self.filesystem.list_files(request.get("scope", "user")),
            }

        if cmd == "fs_read":
            result = self.filesystem.read_file(request.get("path", ""), request.get("scope", "user"))
            if result is None:
                return self._ok("fs_not_found", error="fs_not_found")
            return {
                "status": "ok",
                "message": "fs_read",
                "reboot_required": False,
                "applied": False,
                "file": result,
            }

        return self._ok("unknown_command", error="unknown_command")

    def _enter_maintenance(self, reason=""):
        self._ensure_streaming_services()
        self.mode = "maintenance"
        self.maintenance_reason = str(reason or "")
        self._stop_scan()
        self._ensure_maintenance_services()
        self.latest_imu = None
        self.latest_battery = None
        self.update_led_state()
        return {
            "status": "ok",
            "message": "maintenance_entered",
            "mode": self.mode,
            "scan_stopped": not self.scan_ready,
            "reboot_required": False,
            "applied": True,
        }

    def _exit_maintenance(self):
        self.mode = "normal"
        self.maintenance_reason = ""
        self._release_maintenance_services()
        if self.hardware_ready:
            self._start_scan_if_configured()
        self.update_led_state()
        return {
            "status": "ok",
            "message": "maintenance_exited",
            "mode": self.mode,
            "reboot_required": False,
            "applied": True,
        }

    def _maintenance_sample_cell(self, request):
        if not self._in_maintenance():
            return self._ok("maintenance_required", error="maintenance_required")
        if not self._matrix_configured():
            return self._ok("matrix_layout_required", error="matrix_layout_required")
        analog_pin = int(request["analog_pin"])
        select_pin = int(request["select_pin"])
        duration_ms = int(request.get("duration_ms", 1000))
        level = request.get("level", None)
        try:
            self._start_scan_if_configured()
            avg_mv = self.vdboard.scan.sample_cell_mv(analog_pin, select_pin, duration_ms)
            if avg_mv is None:
                return self._ok("calibration_no_samples", error="calibration_no_samples")
            if level is not None and self.calibration is not None:
                self.calibration.set_point(analog_pin, select_pin, float(level), avg_mv)
            return {
                "status": "ok",
                "message": "calibration_cell_sampled",
                "avg_mv": avg_mv,
                "analog_pin": analog_pin,
                "select_pin": select_pin,
                "duration_ms": duration_ms,
                "reboot_required": False,
                "applied": True,
            }
        finally:
            self._stop_scan()
            gc.collect()

    def _maintenance_sample_all(self, request):
        if not self._in_maintenance():
            return self._ok("maintenance_required", error="maintenance_required")
        if not self._matrix_configured():
            return self._ok("matrix_layout_required", error="matrix_layout_required")
        level = float(request["level"])
        duration_ms = int(request.get("duration_ms", 3000))
        self._start_scan_if_configured()
        try:
            end_ms = time.ticks_add(time.ticks_ms(), max(1, duration_ms))
            active_rows = self._active_rows()
            active_cols = self._active_cols()
            sensor_count = len(active_rows) * len(active_cols)
            sums = [0.0] * sensor_count
            counts = [0] * sensor_count
            while time.ticks_diff(end_ms, time.ticks_ms()) > 0:
                self.vdboard.scan.service()
                frame_view = self.vdboard.scan.pop_frame_mv()
                if frame_view is None:
                    time.sleep_ms(1)
                    continue
                frame_info = self.decode_scan_frame(bytes(frame_view))
                payload = frame_info["payload_mv"]
                for idx, value in enumerate(payload[:sensor_count]):
                    sums[idx] += float(value)
                    counts[idx] += 1
            if not counts or min(counts) == 0:
                return self._ok("calibration_no_samples", error="calibration_no_samples")
            idx = 0
            for analog_pin in active_rows:
                for select_pin in active_cols:
                    avg_mv = sums[idx] / float(counts[idx])
                    self.calibration.set_point(analog_pin, select_pin, level, avg_mv)
                    idx += 1
            return {
                "status": "ok",
                "message": "calibration_all_sampled",
                "level": "{:.3f}".format(level),
                "samples_min": min(counts),
                "samples_max": max(counts),
                "cells": sensor_count,
                "reboot_required": False,
                "applied": True,
            }
        finally:
            self._stop_scan()
            gc.collect()

    def _log_tail(self, request):
        return {
            "status": "ok",
            "message": "log_tail",
            "reboot_required": False,
            "applied": False,
            "lines": self.logger.read_tail(int(request.get("max_lines", 50))),
        }

    def _set_logging(self, request):
        logging_cfg = self._normalize_logging_config(request)
        runtime = self.config_store.update_runtime({"logging": logging_cfg})
        self.runtime = runtime
        self._apply_logging_config(logging_cfg)
        return {
            "status": "ok",
            "message": "logging_updated",
            "reboot_required": False,
            "applied": True,
            "logging": self._logging_status(),
        }

    def _release_url(self, request):
        if request.get("release_url"):
            return request.get("release_url")
        runtime = getattr(self, "runtime", {}) if isinstance(getattr(self, "runtime", {}), dict) else {}
        release_url = runtime.get("update", {}).get("release_url", "")
        return release_url or getattr(config, "DEFAULT_RELEASE_URL", getattr(config, "GITHUB_RELEASE_URL", ""))

    def _ensure_recovery_writer(self):
        if self.recovery_writer is None:
            from update_writer import ManifestTargetWriter
            self.recovery_writer = ManifestTargetWriter(
                "recovery",
                ".",
                self.logger,
                progress=self._target_write_progress,
            )
        return self.recovery_writer

    def _check_recovery_release(self, request):
        release_url = self._release_url(request)
        writer = self._ensure_recovery_writer()
        result = writer.check_release(release_url)
        result["release_url"] = release_url
        self._set_update_state({
            "phase": "ready",
            "operation": "check_recovery_release",
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

    def _write_recovery(self, request):
        release_url = self._release_url(request)
        writer = self._ensure_recovery_writer()
        result = writer.write_release(release_url)
        result["release_url"] = release_url
        installed_version = result.get("version", "")
        if installed_version:
            self.runtime = self.config_store.update_runtime({"system": {"recovery_version": installed_version}})
        self._set_update_state({
            "phase": "done",
            "operation": "write_recovery",
            "version": result.get("version", ""),
            "total_files": int(result.get("downloaded_files", 0)) + int(result.get("skipped_files", 0)),
            "applied_files": int(result.get("downloaded_files", 0)) + int(result.get("skipped_files", 0)),
            "downloaded_files": int(result.get("downloaded_files", 0)),
            "skipped_files": int(result.get("skipped_files", 0)),
            "deleted_files": int(result.get("deleted_files", 0)),
            "current_file": "",
            "last_error": "",
            "last_result": "applied",
            "reboot_required": bool(result.get("reboot_required", True)),
        })
        result["update_state"] = self._current_update_state()
        return result

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

    def _target_write_progress(self, payload):
        raw_phase = str(payload.get("phase", "") or "")
        downloaded = int(payload.get("written_files", payload.get("downloaded_files", 0)) or 0)
        skipped = int(payload.get("skipped_files", 0) or 0)
        total = int(payload.get("total_files", downloaded + skipped) or 0)
        operation = str(payload.get("operation", "write_recovery") or "write_recovery")
        phase = "done" if raw_phase == "complete" else "downloading"
        last_result = "applied" if raw_phase == "complete" else raw_phase
        self._set_update_state({
            "phase": phase,
            "operation": operation,
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

    def _normalize_logging_config(self, source):
        enabled = self._normalize_logging_enabled(source.get("enabled", True))
        capacity = str(source.get("capacity", "default") or "default")
        if capacity not in ("default", "extended"):
            capacity = "default"
        return {
            "enabled": bool(enabled),
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
        logging_cfg = self._normalize_logging_config(logging_cfg or {})
        if hasattr(self.logger, "configure"):
            self.logger.configure(
                enabled=logging_cfg.get("enabled", True),
                capacity=logging_cfg.get("capacity", "default"),
            )
        return logging_cfg

    def _logging_status(self):
        if hasattr(self.logger, "settings"):
            return self.logger.settings()
        return self._normalize_logging_config(self.runtime.get("logging", {}))

    def _recovery_version(self):
        runtime = getattr(self, "runtime", {}) if isinstance(getattr(self, "runtime", {}), dict) else {}
        runtime_version = runtime.get("system", {}).get("recovery_version", "")
        if runtime_version:
            return runtime_version
        try:
            import immutable_config as recovery_config
            return getattr(
                recovery_config,
                "RECOVERY_VERSION",
                getattr(recovery_config, "FIRMWARE_VERSION", "unknown"),
            )
        except Exception:
            return getattr(config, "RECOVERY_VERSION", getattr(config, "RECOVERY_FIRMWARE_VERSION", "unknown"))

    def _system_status(self):
        return {
            "name": self.device_name,
            "hardware_model": getattr(config, "HARDWARE_MODEL", "unknown"),
            "runtime_version": getattr(config, "RUNTIME_VERSION", "unknown"),
            "mode": self.mode,
            "os_version": getattr(config, "FIRMWARE_VERSION", "unknown"),
            "recovery_version": self._recovery_version(),
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
            "native": self.vdboard.scan.memory_stats() if (
                self.vdboard is not None and hasattr(self.vdboard.scan, "memory_stats")
            ) else {},
        }

    def _file_call(self, fn, *args):
        try:
            return fn(*args)
        except ValueError as exc:
            return self._ok(str(exc), error=str(exc))

    def _status(self):
        return {
            "status": "ok",
            "message": "status",
            "mode": self.mode,
            "maintenance_reason": self.maintenance_reason,
            "reboot_required": self.reboot_required,
            "applied": False,
            "device_id": "0x{:08X}".format(self.device_id),
            "device_uid": self.device_uid,
            "device_name": self.device_name,
            "system": self._system_status(),
            "memory": self._memory_status(),
            "wifi_state": self.wifi.state,
            "wifi_setup": self.wifi.portal_status(),
            "ntp": self.time_sync.status() if self.time_sync is not None else {},
            "filter": self.config_store.load_filter(),
            "runtime": self.config_store.load_runtime(),
            "logging": self._logging_status(),
            "update_state": self._current_update_state(),
            "calibration_levels": self.calibration.list_levels() if self.calibration is not None else [],
            "available_rows": list(config.AVAILABLE_ROWS),
            "available_cols": list(config.AVAILABLE_COLS),
            "active_rows": self._active_rows(),
            "active_cols": self._active_cols(),
            "matrix_configured": self._matrix_configured(),
            "matrix_shape": {"rows": len(self._active_rows()), "cols": len(self._active_cols())},
            "sent_packets": self.sent_packets,
            "failed_sends": self.failed_sends,
            "scan": self.vdboard.scan.stats() if self.vdboard is not None else (),
            "stream": self.vdboard.scan.stream_stats() if (
                self.vdboard is not None and hasattr(self.vdboard.scan, "stream_stats")
            ) else {},
        }

    def _maintenance_status(self):
        status = self._status()
        status["message"] = "maintenance_status"
        status["scan_stopped"] = not self.scan_ready
        return status

    def _ok(self, message, applied=False, reboot_required=False, error=""):
        return {
            "status": "ok" if not error else "error",
            "message": message,
            "reboot_required": reboot_required,
            "applied": applied,
            "error": error,
        }

    def _in_maintenance(self):
        return getattr(self, "mode", "normal") == "maintenance"

    def _maintenance_allowed_commands(self):
        return (
            "status",
            "query",
            "maintenance_status",
            "exit_maintenance",
            "reboot",
            "reboot_to_recovery",
            "calibration_sample_cell",
            "calibration_sample_all",
            "calibration_save",
            "dump_calibration",
            "delete_calibration_level",
            "file_list",
            "file_upload_begin",
            "file_upload_chunk",
            "file_upload_finish",
            "file_download_begin",
            "file_download_chunk",
            "file_delete",
            "fs_list",
            "fs_read",
            "set_logging",
            "log_tail",
            "log_read_tail",
            "log_clear",
        )

    def _maintenance_service_commands(self):
        return (
            "calibration_sample_cell",
            "calibration_sample_all",
            "calibration_save",
            "dump_calibration",
            "delete_calibration_level",
            "file_list",
            "file_upload_begin",
            "file_upload_chunk",
            "file_upload_finish",
            "file_download_begin",
            "file_download_chunk",
            "file_delete",
            "fs_list",
            "fs_read",
            "log_tail",
            "log_read_tail",
            "log_clear",
        )

    def _print_setup(self):
        self.logger.info(
            "boot mode=normal device={} id={} version={} matrix={}x{} active={}x{} wifi_setup={}".format(
                self.device_name,
                hex(self.device_id),
                getattr(config, "FIRMWARE_VERSION", "unknown"),
                config.ROWS,
                config.COLS,
                len(self._active_rows()),
                len(self._active_cols()),
                bool(self.wifi_setup_requested),
            )
        )

    def _has_network_hint(self):
        network_cfg = self.config_store.load_network()
        return bool(network_cfg.get("ssid"))

    def _init_scan_backend(self):
        self._ensure_vdboard()
        scan_timing = self.runtime.get("scan_timing", {})
        active_rows = self._active_rows()
        active_cols = self._active_cols()
        self.vdboard.scan.init(
            rows=len(active_rows),
            cols=len(active_cols),
            row_pins=active_rows,
            col_pins=active_cols,
            fps=scan_timing.get("target_fps", config.TARGET_FPS),
            settle_us=scan_timing.get("settle_us", config.MATRIX_SETTLE_US),
            buffer_frames=self.runtime.get("buffer_frames", 8),
            core_id=scan_timing.get("core_id", 1),
        )
        self.native_streaming = self._native_stream_available()
        if self.native_streaming:
            self._configure_native_stream()
        else:
            self._ensure_fallback_scan_services()

    def _ensure_runtime_hardware(self):
        if self.hardware_ready:
            return
        self._ensure_streaming_services()

        self._start_scan_if_configured()

        if self.battery:
            self.logger.info("setup_battery_begin_start")
            self.battery.begin()
            self.logger.info("setup_battery_begin_done")

        if self.imu:
            self.logger.info("setup_imu_begin_start")
            self.imu.begin()
            self.logger.info("setup_imu_begin_done")

        self.hardware_ready = True

    def _clear_tx_buffer(self):
        if self.tx_buffer is not None and hasattr(self.tx_buffer, "clear"):
            try:
                self.tx_buffer.clear()
            except Exception:
                pass

    def _active_rows(self):
        return list(self._matrix_layout().get("active_rows", []))

    def _active_cols(self):
        return list(self._matrix_layout().get("active_cols", []))

    def _matrix_layout(self):
        layout = self.runtime.get("matrix_layout", {})
        try:
            rows, cols = self._validate_matrix_layout(
                layout.get("active_rows", []),
                layout.get("active_cols", []),
            )
        except ValueError:
            return {"active_rows": [], "active_cols": []}
        return {"active_rows": rows, "active_cols": cols}

    def _matrix_configured(self):
        layout = self._matrix_layout()
        return bool(layout["active_rows"] and layout["active_cols"])

    def _validate_matrix_layout(self, rows, cols):
        rows = self._normalize_pin_list(rows, config.AVAILABLE_ROWS, "active_rows")
        cols = self._normalize_pin_list(cols, config.AVAILABLE_COLS, "active_cols")
        if bool(rows) != bool(cols):
            raise ValueError("rows_and_cols_must_both_be_empty_or_set")
        return rows, cols

    def _normalize_pin_list(self, values, allowed, name):
        if not isinstance(values, list):
            raise ValueError("{}_must_be_list".format(name))
        normalized = []
        seen = {}
        allowed_map = {int(pin): True for pin in allowed}
        for value in values:
            pin = int(value)
            if pin not in allowed_map:
                raise ValueError("{}_contains_unavailable_pin".format(name))
            if pin in seen:
                continue
            normalized.append(pin)
            seen[pin] = True
        return normalized

    def _rebuild_matrix_pipeline(self):
        active_rows = self._active_rows()
        active_cols = self._active_cols()
        if self._native_stream_available():
            self.packet = None
            self.filter_chain = None
            self.tx_buffer = None
            self.native_streaming = True
            return

        from packet import PacketBuilder
        from filter_engine import FilterChain

        self.packet = PacketBuilder(active_rows=active_rows, active_cols=active_cols)
        self.filter_chain = FilterChain(
            sensor_count=len(active_rows) * len(active_cols),
            enabled=self.filter_config.get("enabled", False),
            median=self.filter_config.get("median", 3),
            alpha=self.filter_config.get("alpha", 0.25),
        )
        self.native_streaming = False

    def _start_scan_if_configured(self):
        if not self._matrix_configured():
            self.scan_ready = False
            self.logger.info("setup_scan_skipped_no_layout")
            return
        self.logger.info("setup_scan_backend_init_start")
        self._init_scan_backend()
        self.logger.info("setup_scan_backend_init_done")
        self.logger.info("setup_scan_start_start")
        self.vdboard.scan.start()
        self.logger.info("setup_scan_start_done")
        self.scan_ready = True

    def _stop_scan(self):
        if self.vdboard is not None:
            try:
                self.vdboard.scan.stop()
            except Exception as exc:
                self.logger.warn("scan_stop_failed {}".format(exc))
        self.scan_ready = False
        self.latest_matrix = None
        self.latest_frame = None
        self._clear_tx_buffer()

    def _apply_matrix_layout(self):
        self._stop_scan()
        if self.hardware_ready and not self._in_maintenance():
            self._start_scan_if_configured()

    def _ensure_led(self):
        if self.led is not None or not config.ENABLE_LED:
            return
        from sk6812 import SK6812Status
        self.led = SK6812Status()

    def _ensure_vdboard(self):
        if self.vdboard is None:
            import vdboard
            self.vdboard = vdboard

    def _native_stream_available(self):
        return bool(
            self.vdboard is not None
            and hasattr(self.vdboard.scan, "pop_packet")
            and hasattr(self.vdboard.scan, "set_packet_options")
            and hasattr(self.vdboard.scan, "load_calibration")
        )

    def _configure_native_stream(self):
        if not self._native_stream_available():
            self.native_streaming = False
            self._ensure_fallback_scan_services()
            return False
        try:
            import secrets
            self.vdboard.scan.set_packet_options(
                self.device_id,
                bool(getattr(config, "USE_HMAC", False)),
                int(getattr(config, "HMAC_LEN", 0)),
                getattr(secrets, "HMAC_KEY", b""),
            )
            self._configure_native_filter()
            self._reload_native_calibration()
            self._update_native_imu_cache()
            self._update_native_battery_cache()
            self.native_streaming = True
            return True
        except Exception as exc:
            self.logger.warn("native_stream_config_failed {}".format(exc))
            self.native_streaming = False
            self._ensure_fallback_scan_services()
            return False

    def _configure_native_filter(self):
        if not self._native_stream_available() or not hasattr(self.vdboard.scan, "configure_filter"):
            return False
        self.vdboard.scan.configure_filter(
            self.filter_config.get("enabled", False),
            self.filter_config.get("median", 3),
            self.filter_config.get("alpha", 0.25),
        )
        return True

    def _reload_native_calibration(self):
        if not self._native_stream_available():
            return False
        table = self._native_calibration_table()
        self.vdboard.scan.load_calibration(table)
        return True

    def _native_calibration_table(self):
        active_rows = self._active_rows()
        active_cols = self._active_cols()
        if not active_rows or not active_cols:
            return []
        from calibration_store import CalibrationStore
        store = CalibrationStore(config.CALIBRATION_DIR)
        points = store.load() or {}
        table = []
        for analog_pin in active_rows:
            for select_pin in active_cols:
                sensor_key = "{}:{}".format(int(analog_pin), int(select_pin))
                sensor_points = points.get(sensor_key, {})
                curve = []
                for level_key, sample_mv in sensor_points.items():
                    curve.append((float(sample_mv), float(level_key)))
                curve.sort(key=lambda item: item[0])
                table.append(curve)
        if self.calibration is None:
            try:
                import sys
                sys.modules.pop("calibration_store", None)
            except Exception:
                pass
        return table

    def _update_native_imu_cache(self):
        if self._native_stream_available() and hasattr(self.vdboard.scan, "update_imu_cache"):
            self.vdboard.scan.update_imu_cache(self.latest_imu)

    def _update_native_battery_cache(self):
        if self._native_stream_available() and hasattr(self.vdboard.scan, "update_battery_cache"):
            self.vdboard.scan.update_battery_cache(self.latest_battery)

    def _ensure_streaming_services(self):
        if self.mqtt_transport is not None:
            return
        from mqtt_transport import MQTTTransport
        from time_sync import TimeSync
        from utils import RateCounter

        self.mqtt_transport = MQTTTransport(lambda: self.runtime, self.device_uid, self.logger)
        self.time_sync = TimeSync(self.runtime.get("ntp_servers", []))
        self.scan_rate = RateCounter(1000)

        if config.ENABLE_IMU:
            from bmi270 import BMI270
            self.imu = BMI270()
        if config.ENABLE_BATTERY:
            from bq25180 import BQ25180
            self.battery = BQ25180()

    def _ensure_maintenance_services(self):
        if self.filesystem is None:
            from filesystem_api import FilesystemAPI
            self.filesystem = FilesystemAPI(
                getattr(config, "DATA_FILES_DIR", "data/files"),
                getattr(config, "DATA_TMP_DIR", "data/tmp"),
                {
                    "user": getattr(config, "DATA_FILES_DIR", "data/files"),
                    "logs": getattr(config, "DATA_LOG_DIR", "data/logs"),
                    "calibration": getattr(config, "CALIBRATION_DIR", "device_state/calibration"),
                },
            )
        if self.calibration is None:
            from calibration_store import CalibrationStore
            self.calibration = CalibrationStore(config.CALIBRATION_DIR)
            self.calibration.load()

    def _release_maintenance_services(self):
        self.filesystem = None
        self.calibration = None
        for module_name in ("filesystem_api", "calibration_store"):
            try:
                import sys
                sys.modules.pop(module_name, None)
            except Exception:
                pass
        gc.collect()

    def _ensure_fallback_scan_services(self):
        if self.packet is not None and self.filter_chain is not None and self.decode_scan_frame is not None:
            return
        from calibration_store import CalibrationStore
        from frame_protocol import decode_scan_frame
        from packet import PacketBuilder
        from packet_buffer import PacketBuffer
        from filter_engine import FilterChain

        active_rows = self._active_rows()
        active_cols = self._active_cols()
        if self.calibration is None:
            self.calibration = CalibrationStore(config.CALIBRATION_DIR)
            self.calibration.load()
        self.packet = PacketBuilder(active_rows=active_rows, active_cols=active_cols)
        self.filter_chain = FilterChain(
            sensor_count=len(active_rows) * len(active_cols),
            enabled=self.filter_config.get("enabled", False),
            median=self.filter_config.get("median", 3),
            alpha=self.filter_config.get("alpha", 0.25),
        )
        if config.USE_PACKET_BUFFER:
            self.tx_buffer = PacketBuffer(
                capacity=config.PACKET_BUFFER_SIZE,
                drop_oldest=config.PACKET_BUFFER_DROP_OLDEST
            )
        self.decode_scan_frame = decode_scan_frame
        self.native_streaming = False

    def _ensure_runtime_services(self):
        self._ensure_streaming_services()
        self._ensure_maintenance_services()
        if not self._native_stream_available():
            self._ensure_fallback_scan_services()
