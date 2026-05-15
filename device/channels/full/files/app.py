# app.py
import machine
import time
import gc

import config
import board_pins
import vdboard

from calibration_store import CalibrationStore
from device_identity import get_device_id, get_device_name, get_device_uid
from device_logging import DeviceLogger
from filesystem_api import FilesystemAPI
from filter_engine import FilterChain
from frame_protocol import decode_scan_frame
from packet import PacketBuilder
from packet_buffer import PacketBuffer
from runtime_config import RuntimeConfigStore
from time_sync import TimeSync
from udp_control import UDPControlServer
from update_manager import UpdateManager
from utils import RateCounter
from wifi_manager import WiFiManager

if config.ENABLE_IMU:
    from bmi270 import BMI270

if config.ENABLE_BATTERY:
    from bq25180 import BQ25180

if config.ENABLE_LED:
    from sk6812 import SK6812Status


class App:
    def __init__(self, wifi_setup_requested=False):
        self.wifi_setup_requested = wifi_setup_requested
        self.frame_id = 0
        self.device_id = get_device_id()
        self.device_uid = get_device_uid()
        self.device_name = get_device_name(config.DEVICE_NAME)
        self.reboot_required = False
        self.calibration_mode = False
        self.last_connect_ms = 0
        self.boot_network_initialized = False
        self.hardware_ready = False
        self.scan_ready = False

        self.config_store = RuntimeConfigStore(config.DEVICE_STATE_DIR)
        self.logger = DeviceLogger(config.LOG_PATH)
        self.filesystem = FilesystemAPI(config.DEVICE_STATE_DIR)
        self.runtime = self.config_store.load_runtime()
        self.network = self.config_store.load_network()
        self.filter_config = self.config_store.load_filter()

        self.wifi = WiFiManager(self.config_store, self.logger)
        self.update_manager = UpdateManager(self.config_store, self.logger, ".")
        self.control = UDPControlServer(config.UDP_CONTROL_PORT, self.logger)
        self.time_sync = TimeSync(self.runtime.get("ntp_servers", []))
        self.calibration = CalibrationStore(config.CALIBRATION_DIR)
        self.calibration.load()

        if config.USE_PACKET_BUFFER:
            self.tx_buffer = PacketBuffer(
                capacity=config.PACKET_BUFFER_SIZE,
                drop_oldest=config.PACKET_BUFFER_DROP_OLDEST
            )
        else:
            self.tx_buffer = None

        self.imu = BMI270() if config.ENABLE_IMU else None
        self.battery = BQ25180() if config.ENABLE_BATTERY else None
        self.led = SK6812Status() if config.ENABLE_LED else None
        self.udp = None

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

        self.packet = None
        self.filter_chain = None
        self._rebuild_matrix_pipeline()

        self.scan_rate = RateCounter(1000)
        self.sent_packets = 0
        self.failed_sends = 0

    def setup(self):
        self.logger.info("setup_start")
        self._print_setup()

        if self.led:
            self.logger.info("setup_led_begin_start")
            self.led.begin()
            self.logger.info("setup_led_begin_done")
            self.logger.info("setup_led_boot_window_start")
            self.led.set_boot_window()
            self.logger.info("setup_led_boot_window_done")

        if config.PRINT_PIN_CONFLICTS:
            conflicts = board_pins.validate_pins()
            if conflicts:
                for pin, names in conflicts.items():
                    self.logger.warn("pin_conflict gpio={} roles={}".format(pin, names))

        if self.wifi_setup_requested or not self._has_network_hint():
            if self.led:
                self.led.set_wifi_setup()
            reason = "boot_window" if self.wifi_setup_requested else "missing_credentials"
            self.wifi.start_setup_portal(reason)
            self.logger.info("setup_wifi_portal_started reason={}".format(reason))
            wifi_ok = False
        else:
            self.logger.info("setup_wifi_connect_start")
            wifi_ok = self.wifi.connect()
            self.logger.info("setup_wifi_connect_done ok={}".format(bool(wifi_ok)))
            if not wifi_ok:
                if self.led:
                    self.led.set_wifi_setup()
                self.wifi.start_setup_portal("connect_failed")
                self.logger.info("setup_wifi_portal_started reason=connect_failed")
        self._ensure_udp_data_socket(wifi_ok)
        self.logger.info("setup_udp_socket_done ok={}".format(bool(wifi_ok)))
        self.control.begin()
        self.logger.info("setup_control_begin_done")

        if wifi_ok:
            self.logger.info("setup_ntp_sync_start")
            self._sync_time()
            self.logger.info("setup_ntp_sync_done")
            self.logger.info("setup_boot_update_check_start")
            self._check_updates_on_boot()
            self.logger.info("setup_boot_update_check_done")
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
            self.control.poll(self._handle_control_request)
            update_result = self.update_manager.service()
            if update_result is not None:
                self._handle_update_result(update_result, now)
                self._announce_status(now, force=True)

            if self.wifi.is_connected() and not self.boot_network_initialized:
                self.boot_network_initialized = True
                self._ensure_udp_data_socket(True)
                self._sync_time()
                self._check_updates_on_boot()
                self._announce_status(now, force=True)
                self.update_led_state()

            if not self.hardware_ready and not self.wifi.setup_active():
                self._ensure_runtime_hardware()
                self.update_led_state()

            if self.hardware_ready and self.imu and time.ticks_diff(now, self.last_imu_ms) >= imu_interval:
                self.last_imu_ms = now
                self.latest_imu = self.imu.read()

            if self.hardware_ready and self.battery and time.ticks_diff(now, self.last_battery_ms) >= battery_interval:
                self.last_battery_ms = now
                self.latest_battery = self.battery.read_status()

            if self.scan_ready and not self.calibration_mode:
                vdboard.scan.service()
                self.handle_scan(now)

            self.handle_transmit()

            if (not self.wifi.is_connected()
                    and not self.wifi.setup_active()
                    and self._has_network_hint()
                    and time.ticks_diff(now, self.last_connect_ms) >= 10000):
                self.last_connect_ms = now
                wifi_ok = self.wifi.connect()
                self._ensure_udp_data_socket(wifi_ok)
                if wifi_ok:
                    self.update_led_state()
                else:
                    self.wifi.start_setup_portal("reconnect_failed")
                    self.update_led_state()

            if self.led and time.ticks_diff(now, self.last_led_ms) >= led_interval:
                self.last_led_ms = now
                self.led.update()

            if not self.time_sync.status()["synced"] and time.ticks_diff(now, self.last_ntp_retry_ms) >= 60000:
                self.last_ntp_retry_ms = now
                if self.wifi.is_connected():
                    self._sync_time()

            self._announce_status(now)

            if self.reboot_required and self._reboot_due(now):
                self.logger.warn("reboot_required_restart")
                time.sleep_ms(250)
                machine.reset()

            if config.GC_EVERY_N_FRAMES > 0 and self.frame_id > 0:
                if self.frame_id % config.GC_EVERY_N_FRAMES == 0:
                    gc.collect()

    def handle_scan(self, timestamp_ms):
        frame_view = vdboard.scan.pop_frame_mv()
        if frame_view is None:
            return
        frame_info = decode_scan_frame(bytes(frame_view))
        raw_matrix = frame_info["payload_mv"]
        self.latest_frame = frame_info
        self.latest_matrix = self._apply_sensor_pipeline(raw_matrix)

        send_every = self.runtime.get("scan_timing", {}).get(
            "send_every_n_frames",
            config.SEND_EVERY_N_FRAMES
        )
        self.frame_id = int(frame_info["seq"])
        if send_every > 1 and (self.frame_id % send_every != 0):
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

    def handle_transmit(self):
        if not config.USE_PACKET_BUFFER or self.tx_buffer is None:
            return

        if self.udp is None or not self.wifi.is_connected():
            self.tx_buffer.clear()
            return

        max_send = config.SEND_MAX_PER_LOOP
        for _ in range(max_send):
            packet = self.tx_buffer.pop()
            if packet is None:
                return
            if not self.send_packet(packet):
                return

    def send_packet(self, packet):
        if self.udp is None or not self.wifi.is_connected():
            return False

        ok = self.udp.send(packet)
        if ok:
            self.sent_packets += 1
            return True

        self.failed_sends += 1
        return False

    def update_led_state(self):
        if not self.led:
            return
        if self.calibration_mode:
            self.led.set_calibration()
            return
        if self.wifi.setup_active():
            self.led.set_wifi_setup()
            return
        if self.reboot_required:
            self.led.set_updating()
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

    def _ensure_udp_data_socket(self, wifi_ok):
        if not wifi_ok:
            self.udp = None
            return
        from udp_stream import UDPStreamer
        data_server = self.runtime.get("data_server", {})
        self.udp = UDPStreamer(
            data_server.get("host", config.UDP_SERVER_IP),
            data_server.get("port", config.UDP_SERVER_PORT)
        )

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
        synced = self.time_sync.sync()
        if synced:
            self.logger.info("ntp_sync_ok epoch={}".format(self.time_sync.now_epoch()))
        else:
            self.logger.warn("ntp_sync_failed error={}".format(self.time_sync.last_error))

    def _reboot_due(self, now):
        if self.reboot_deadline_ms is None:
            return True
        return time.ticks_diff(now, self.reboot_deadline_ms) >= 0

    def _handle_update_result(self, result, now):
        if (result.get("status") == "ok"
                and result.get("message") == "update_applied"
                and result.get("reboot_required")):
            self.reboot_required = True
            if self.reboot_deadline_ms is None:
                self.reboot_deadline_ms = time.ticks_add(now, 1200)

    def _announce_status(self, now=None, force=False):
        if self.control.sock is None or not self.wifi.is_connected():
            return False
        if now is None:
            now = time.ticks_ms()
        if (not force
                and time.ticks_diff(now, self.last_status_announce_ms) < config.STATUS_ANNOUNCE_INTERVAL_MS):
            return False
        master = self.runtime.get("master_server", {})
        host = master.get("host", "")
        if not host:
            return False
        port = int(master.get("port", config.UDP_CONTROL_PORT))
        payload = self._status()
        payload["message"] = "status_announce"
        try:
            ok = self.control.send(host, port, payload)
        except OSError as exc:
            self.logger.warn("status_announce_failed {}".format(exc))
            return False
        if ok:
            self.last_status_announce_ms = now
        return ok

    def _check_updates_on_boot(self):
        runtime = self.config_store.load_runtime()
        update_cfg = runtime.get("update", {})
        if not update_cfg.get("check_on_boot", True):
            return
        if self.led:
            self.led.set_updating()
        try:
            result = self.update_manager.check()
            self.logger.info("update_check {}".format(result.get("message")))
        except Exception as exc:
            self.logger.warn("update_check_failed {}".format(exc))
        finally:
            self.update_led_state()

    def _apply_runtime_reload(self):
        self.runtime = self.config_store.load_runtime()
        self.filter_config = self.config_store.load_filter()
        self._apply_matrix_layout()
        self.time_sync.servers = list(self.runtime.get("ntp_servers", []))
        self._ensure_udp_data_socket(self.wifi.is_connected())

    def _authorized(self, addr):
        master = self.config_store.load_runtime().get("master_server", {})
        host = master.get("host", "")
        port = int(master.get("port", config.UDP_CONTROL_PORT))
        if not host:
            return True
        return addr[0] == host and int(addr[1]) == port

    def _handle_control_request(self, request, addr):
        if not self._authorized(addr):
            self.logger.warn("control_unauthorized from={}:{}".format(addr[0], addr[1]))
            return None

        cmd = request.get("command", request.get("cmd", "")).strip().lower()
        if not cmd:
            return self._ok("missing_command", error="missing_command")

        if cmd in ("status", "query"):
            return self._status()

        if cmd == "reload_config":
            self._apply_runtime_reload()
            return self._ok("config_reloaded", applied=True)

        if cmd == "check_update":
            return self.update_manager.start_check()

        if cmd == "apply_update":
            return self.update_manager.start_apply()

        if cmd == "set_servers":
            runtime = self.config_store.update_runtime({
                "master_server": request.get("master_server", {}),
                "data_server": request.get("data_server", {}),
            })
            self.runtime = runtime
            self._ensure_udp_data_socket(self.wifi.is_connected())
            return self._ok("servers_updated", applied=True)

        if cmd == "set_filter":
            filter_data = self.config_store.update_filter(request.get("filter", {}))
            self.filter_chain.apply_config(
                filter_data.get("enabled", False),
                filter_data.get("median", 3),
                filter_data.get("alpha", 0.25),
            )
            return self._ok("filter_updated", applied=True)

        if cmd == "set_matrix_layout":
            try:
                rows, cols = self._validate_matrix_layout(
                    request.get("active_rows", []),
                    request.get("active_cols", []),
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

        if cmd == "enter_calibration_mode":
            self.calibration_mode = bool(request.get("enabled", True))
            self.update_led_state()
            return self._ok("calibration_mode_updated", applied=True)

        if cmd == "start_calibration":
            if not self.scan_ready:
                return self._ok("matrix_layout_required", error="matrix_layout_required")
            if not self.calibration_mode:
                return self._ok("calibration_mode_required", error="calibration_mode_required")
            analog_pin = int(request["analog_pin"])
            select_pin = int(request["select_pin"])
            level = float(request["level"])
            start_delay = int(request.get("start_delay_ms", 0))
            duration_ms = int(request.get("duration_ms", 1000))
            if start_delay > 0:
                time.sleep_ms(start_delay)
            avg_mv = vdboard.scan.sample_cell_mv(analog_pin, select_pin, duration_ms)
            if avg_mv is None:
                return self._ok("calibration_no_samples", error="calibration_no_samples")
            self.calibration.set_point(analog_pin, select_pin, level, avg_mv)
            self.calibration.save()
            return {
                "status": "ok",
                "message": "calibration_sampled",
                "avg_mv": avg_mv,
                "reboot_required": False,
                "applied": True,
            }

        if cmd == "calibrate_all":
            if not self.scan_ready:
                return self._ok("matrix_layout_required", error="matrix_layout_required")
            if not self.calibration_mode:
                return self._ok("calibration_mode_required", error="calibration_mode_required")
            level = float(request["level"])
            start_delay = int(request.get("start_delay_ms", 0))
            duration_ms = int(request.get("duration_ms", 5000))
            if start_delay > 0:
                time.sleep_ms(start_delay)
            end_ms = time.ticks_add(time.ticks_ms(), max(1, duration_ms))
            active_rows = self._active_rows()
            active_cols = self._active_cols()
            sensor_count = len(active_rows) * len(active_cols)
            sums = [0.0] * sensor_count
            counts = [0] * sensor_count
            while time.ticks_diff(end_ms, time.ticks_ms()) > 0:
                vdboard.scan.service()
                frame_view = vdboard.scan.pop_frame_mv()
                if frame_view is None:
                    time.sleep_ms(1)
                    continue
                frame_info = decode_scan_frame(bytes(frame_view))
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
            self.calibration.save()
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

        if cmd == "end_calibration":
            self.calibration_mode = False
            self.calibration.save()
            self.update_led_state()
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
            self.wifi.start_setup_portal("udp_command")
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
            result = self.wifi.apply_credentials(request.get("ssid", ""), request.get("password", ""))
            self._ensure_udp_data_socket(self.wifi.is_connected())
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
            return {
                "status": "ok",
                "message": "log_tail",
                "reboot_required": False,
                "applied": False,
                "lines": self.logger.read_tail(int(request.get("max_lines", 50))),
            }

        if cmd == "fs_list":
            return {
                "status": "ok",
                "message": "fs_list",
                "reboot_required": False,
                "applied": False,
                "items": self.filesystem.list_files(),
            }

        if cmd == "fs_read":
            result = self.filesystem.read_file(request.get("path", ""))
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

    def _status(self):
        return {
            "status": "ok",
            "message": "status",
            "reboot_required": self.reboot_required,
            "applied": False,
            "device_id": "0x{:08X}".format(self.device_id),
            "device_uid": self.device_uid,
            "device_name": self.device_name,
            "wifi_state": self.wifi.state,
            "wifi_setup": self.wifi.portal_status(),
            "ntp": self.time_sync.status(),
            "filter": self.config_store.load_filter(),
            "runtime": self.config_store.load_runtime(),
            "update_state": self.update_manager.status(),
            "calibration_mode": self.calibration_mode,
            "calibration_levels": self.calibration.list_levels(),
            "available_rows": list(config.AVAILABLE_ROWS),
            "available_cols": list(config.AVAILABLE_COLS),
            "active_rows": self._active_rows(),
            "active_cols": self._active_cols(),
            "matrix_configured": self._matrix_configured(),
            "matrix_shape": {"rows": len(self._active_rows()), "cols": len(self._active_cols())},
            "sent_packets": self.sent_packets,
            "failed_sends": self.failed_sends,
            "scan": vdboard.scan.stats(),
        }

    def _ok(self, message, applied=False, reboot_required=False, error=""):
        return {
            "status": "ok" if not error else "error",
            "message": message,
            "reboot_required": reboot_required,
            "applied": applied,
            "error": error,
        }

    def _print_setup(self):
        print("========== Setup ==========")
        print("Device:", self.device_name)
        print("Device ID:", hex(self.device_id))
        print("Matrix:", config.ROWS, "x", config.COLS)
        print("Active rows:", self._active_rows())
        print("Active cols:", self._active_cols())
        print("Wi-Fi setup requested:", self.wifi_setup_requested)

    def _has_network_hint(self):
        network_cfg = self.config_store.load_network()
        return bool(network_cfg.get("ssid"))

    def _init_scan_backend(self):
        scan_timing = self.runtime.get("scan_timing", {})
        active_rows = self._active_rows()
        active_cols = self._active_cols()
        vdboard.scan.init(
            rows=len(active_rows),
            cols=len(active_cols),
            row_pins=active_rows,
            col_pins=active_cols,
            fps=scan_timing.get("target_fps", config.TARGET_FPS),
            settle_us=scan_timing.get("settle_us", config.MATRIX_SETTLE_US),
            buffer_frames=self.runtime.get("buffer_frames", 8),
            core_id=scan_timing.get("core_id", 1),
        )

    def _ensure_runtime_hardware(self):
        if self.hardware_ready:
            return

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
        self.packet = PacketBuilder(active_rows=active_rows, active_cols=active_cols)
        self.filter_chain = FilterChain(
            sensor_count=len(active_rows) * len(active_cols),
            enabled=self.filter_config.get("enabled", False),
            median=self.filter_config.get("median", 3),
            alpha=self.filter_config.get("alpha", 0.25),
        )

    def _start_scan_if_configured(self):
        if not self._matrix_configured():
            self.scan_ready = False
            self.logger.info("setup_scan_skipped_no_layout")
            return
        self.logger.info("setup_scan_backend_init_start")
        self._init_scan_backend()
        self.logger.info("setup_scan_backend_init_done")
        self.logger.info("setup_scan_start_start")
        vdboard.scan.start()
        self.logger.info("setup_scan_start_done")
        self.scan_ready = True

    def _apply_matrix_layout(self):
        if self.scan_ready:
            try:
                vdboard.scan.stop()
            except Exception as exc:
                self.logger.warn("scan_stop_failed {}".format(exc))
        self.scan_ready = False
        self.latest_matrix = None
        self.latest_frame = None
        self._rebuild_matrix_pipeline()
        if self.hardware_ready:
            self._start_scan_if_configured()
