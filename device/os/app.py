# app.py
import machine
import time
import gc

import config
import board_pins

from device_identity import get_device_id, get_device_name, get_device_uid, get_packet_device_uid_bytes
from device_logging import DeviceLogger
from runtime_config import RuntimeConfigStore
from wifi_manager import WiFiManager


class App:
    def __init__(self, wifi_setup_requested=False):
        self.wifi_setup_requested = wifi_setup_requested
        self.frame_id = 0
        self.device_id = get_device_id()
        self.device_uid = get_device_uid()
        self.packet_device_uid = get_packet_device_uid_bytes()
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
        self.scan_packet_buffer = None
        self.last_scan_health = {}
        self.last_matrix_start_failed = False

        self.imu = None
        self.battery = None
        self.led = None
        self.control_transport = None
        self.udp_stream = None
        self.offline_recorder = None
        self.action_button = None
        self.action_button_down_since_ms = None
        self.action_button_handled = False
        self.control_disconnected_since_ms = 0
        self.control_connected_since_ms = 0
        self.offline_led_unavailable_until_ms = 0
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
        self.last_findme_ms = 0
        self.reboot_deadline_ms = None
        self.send_backoff_until_ms = 0
        self.last_gc_frame_id = 0

        self.sent_packets = 0
        self.failed_sends = 0

        if self._has_pending_matrix_layout():
            self.last_matrix_start_failed = True
            self.logger.warn("matrix_layout_pending_recovered scan_disabled")
            self.runtime = self.config_store.update_runtime({
                "matrix_layout": {"active_rows": [], "active_cols": []},
                "matrix_layout_state": {
                    "pending": False,
                    "committed": False,
                    "last_error": "pending_layout_recovered_on_boot",
                },
            })
        elif self._has_interrupted_scan_session():
            self.last_matrix_start_failed = True
            self.logger.warn("matrix_scan_interrupted autostart_disabled")
            self.runtime = self.config_store.update_runtime({
                "matrix_scan_state": {
                    "active": False,
                    "autostart_disabled": True,
                    "last_error": "previous_scan_session_interrupted",
                },
            })

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
            self._log_memory_stage("wifi")
            if not wifi_ok:
                self.logger.warn("setup_wifi_connect_failed_offline")

        self._ensure_led()
        self._ensure_action_button()
        if self.led:
            self.logger.info("setup_led_begin_start")
            self.led.begin()
            if hasattr(self.led, "configure"):
                self.led.configure(self.runtime.get("indicators", {}))
            self.logger.info("setup_led_begin_done")
            self.logger.info("setup_led_boot_window_start")
            self.led.set_boot_window()
            self.logger.info("setup_led_boot_window_done")
            self._log_memory_stage("led")
            if self.wifi.setup_active():
                self.led.set_wifi_setup()

        if wifi_ok:
            self._run_findme("boot")
            self._log_memory_stage("findme")
            self._ensure_streaming_services()

        if wifi_ok:
            self.logger.info("setup_ntp_sync_start")
            self._sync_time()
            self.logger.info("setup_ntp_sync_done")
            self.logger.info("setup_status_announce_start")
            self._announce_status(force=True)
            self.logger.info("setup_status_announce_done")
            self._log_memory_stage("after_status")
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
            if self.control_transport is not None:
                self.control_transport.poll(self.wifi.is_connected(), self._handle_control_request)
            self._service_offline_state(now)
            self._service_action_button(now)
            if self.wifi.is_connected() and not self.boot_network_initialized:
                self.boot_network_initialized = True
                self._run_findme("wifi_connected")
                self._ensure_streaming_services()
                self._sync_time()
                self._announce_status(now, force=True)
                self.update_led_state()
            self._service_findme(now)

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
                    self._run_findme("wifi_reconnect")
                    self.update_led_state()
                else:
                    self.logger.warn("wifi_reconnect_failed_offline")
                    self.update_led_state()

            if self.led and time.ticks_diff(now, self.last_led_ms) >= led_interval:
                self.last_led_ms = now
                self.update_led_state()
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
                self._stop_scan()
                time.sleep_ms(250)
                try:
                    if self.control_transport is not None:
                        self.control_transport.close()
                except Exception:
                    pass
                time.sleep_ms(50)
                machine.reset()

            self._maybe_collect_garbage()

    def handle_scan(self, timestamp_ms):
        if getattr(self, "native_streaming", False) and hasattr(self.vdboard.scan, "pop_packet_into"):
            online = self._stream_online()
            offline_active = self._offline_recording_active()
            if online and self._transmit_backing_off(timestamp_ms):
                online = False
            if not online and not offline_active:
                return
            packet = self._native_pop_packet()
            if packet is None:
                return

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
            if offline_active:
                self.offline_recorder.write_packet(packet, timestamp_ms)
            if online:
                self.send_packet(packet)
            return

        self.logger.warn("scan_native_stream_required")
        self.last_matrix_start_failed = True
        self._stop_scan()

    def handle_transmit(self):
        if self._in_maintenance():
            self._clear_tx_buffer()
            return
        if not config.USE_PACKET_BUFFER or self.tx_buffer is None:
            return

        if not self.wifi.is_connected():
            self.tx_buffer.clear()
            return
        if self.udp_stream is None:
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
        if self.udp_stream is None:
            return False

        ok = self.udp_stream.send(packet, self.wifi.is_connected())
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
        if hasattr(self.led, "set_context"):
            self.led.set_context(self._indicator_context())
        if self.offline_led_unavailable_until_ms and time.ticks_diff(time.ticks_ms(), self.offline_led_unavailable_until_ms) < 0:
            if hasattr(self.led, "set_offline_unavailable"):
                self.led.set_offline_unavailable()
            else:
                self.led.set_error()
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
        if self._findme_rejected() and hasattr(self.led, "set_findme_rejected"):
            self.led.set_findme_rejected()
            return
        if self._findme_gateway_lost() and hasattr(self.led, "set_findme_gateway_lost"):
            self.led.set_findme_gateway_lost()
            return
        if self._findme_no_gateway() and hasattr(self.led, "set_findme_no_gateway"):
            self.led.set_findme_no_gateway()
            return
        if self.wifi.setup_active():
            self.led.set_wifi_setup()
            return
        if self._offline_recording_active() and hasattr(self.led, "set_offline_recording"):
            self.led.set_offline_recording(self.offline_recorder.led_bucket())
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

    def _battery_status(self, refresh=False):
        if not self.battery:
            return {}
        if refresh and self.latest_battery is None:
            self.latest_battery = self.battery.read_status()

        values = self.latest_battery
        status_code = getattr(self.battery, "last_status_code", 0)
        fault = 0
        vbat_mv = 0
        if values:
            try:
                status_code, fault, vbat_mv = values
            except Exception:
                pass

        status_name = self.battery.status_name() if hasattr(self.battery, "status_name") else "unknown"
        charging = self.battery.is_charging() if hasattr(self.battery, "is_charging") else False
        charge_done = self.battery.is_charge_done() if hasattr(self.battery, "is_charge_done") else False
        if charging:
            state = "charging"
        elif charge_done:
            state = "charge_done"
        elif status_name == "not_charging":
            state = "not_charging"
        else:
            state = "unknown"

        return {
            "state": state,
            "status": status_name,
            "status_code": status_code,
            "charging": charging,
            "charge_done": charge_done,
            "fault": fault,
            "vbat_mv": vbat_mv,
        }

    def _indicator_context(self):
        runtime = self.runtime if isinstance(self.runtime, dict) else {}
        scan_timing = runtime.get("scan_timing", {}) or {}
        stream = {}
        if self.vdboard is not None and hasattr(self.vdboard.scan, "stream_stats"):
            try:
                stream = self.vdboard.scan.stream_stats() or {}
            except Exception:
                stream = {}
        offline_status = self._offline_recording_status()
        current_fps = 0
        if self.scan_rate is not None:
            current_fps = getattr(self.scan_rate, "rate", 0)
        latest_imu = self.latest_imu or (0, 0, 0, 0, 0, 0, 0, 0, 0)
        imu_motion = 0
        try:
            imu_motion = abs(float(latest_imu[3])) + abs(float(latest_imu[4])) + abs(float(latest_imu[5]))
            imu_motion = min(1.0, imu_motion / 200.0)
        except Exception:
            imu_motion = 0
        pressure_max = 0
        if self.latest_matrix:
            try:
                pressure_max = max(self.latest_matrix)
            except Exception:
                pressure_max = 0
        return {
            "mode": self.mode,
            "rows": len(self._active_rows()),
            "cols": len(self._active_cols()),
            "scan_active": bool(self.scan_ready and not self._in_maintenance()),
            "scan_ready": bool(self.scan_ready),
            "streaming": bool(self.scan_ready and self._stream_online()),
            "target_fps": int(scan_timing.get("target_fps", getattr(config, "TARGET_FPS", 60))),
            "current_fps": current_fps,
            "control_connected": bool(self.control_transport is not None and self.control_transport.is_connected()),
            "findme": self._findme_status(),
            "findme_no_gateway": self._findme_no_gateway(),
            "findme_gateway_lost": self._findme_gateway_lost(),
            "findme_rejected": self._findme_rejected(),
            "failed_sends": int(self.failed_sends),
            "dropped_frames": int(stream.get("dropped", stream.get("dropped_frames", 0)) or 0),
            "offline_recording": offline_status,
            "online_recording": runtime.get("online_recording", {}) or {},
            "calibration_active": bool(self._in_maintenance() and "calibration" in str(self.maintenance_reason or "")),
            "heap_free": int(gc.mem_free()) if hasattr(gc, "mem_free") else 0,
            "pressure_max": pressure_max,
            "cop_activity": 0,
            "imu_motion": imu_motion,
        }

    def _findme_status(self):
        runtime = self.runtime if isinstance(self.runtime, dict) else {}
        state = dict(runtime.get("findme", {}) or {})
        control_transport = getattr(self, "control_transport", None)
        if control_transport is not None and hasattr(control_transport, "findme_status"):
            transport = control_transport.findme_status()
            if transport.get("state") in ("attached", "rejected", "gateway_lost", "attaching"):
                state["state"] = transport.get("state")
            if transport.get("gateway_id"):
                state["gateway_id"] = transport.get("gateway_id")
            state["session_id"] = transport.get("session_id", state.get("session_id", ""))
            if transport.get("last_error"):
                state["last_error"] = transport.get("last_error")
            state["connected"] = bool(transport.get("connected"))
        return state

    def _findme_no_gateway(self):
        state = self._findme_status()
        if not self.wifi.is_connected():
            return False
        if self.control_transport is not None and self.control_transport.is_connected():
            return False
        return bool(state.get("last_error")) and state.get("state") in ("no_gateway", "idle", "discovered", "")

    def _findme_gateway_lost(self):
        state = self._findme_status()
        if not self.wifi.is_connected():
            return False
        if self.control_transport is not None and self.control_transport.is_connected():
            return False
        return state.get("state") == "gateway_lost" or bool(state.get("last_success_ms") and state.get("host"))

    def _findme_rejected(self):
        state = self._findme_status()
        return state.get("state") == "rejected" or state.get("last_error") == "device_rejected"

    def _indicator_status(self):
        if self.led is None or not hasattr(self.led, "status"):
            return {}
        try:
            return self.led.status()
        except Exception as exc:
            return {"error": str(exc)}

    def _ensure_action_button(self):
        if self.action_button is not None:
            return
        try:
            self.action_button = machine.Pin(board_pins.ACTION_BUTTON_PIN, machine.Pin.IN, machine.Pin.PULL_UP)
        except Exception as exc:
            self.logger.warn("action_button_init_failed {}".format(exc))
            self.action_button = None

    def _button_pressed(self):
        if self.action_button is None:
            return False
        try:
            return int(self.action_button.value()) == 0
        except Exception:
            return False

    def _service_action_button(self, now):
        if self.action_button is None:
            return
        if self._button_pressed():
            if self.action_button_down_since_ms is None:
                self.action_button_down_since_ms = now
                self.action_button_handled = False
                return
            if (not self.action_button_handled
                    and time.ticks_diff(now, self.action_button_down_since_ms) >= int(getattr(config, "ACTION_BUTTON_LONG_PRESS_MS", 3000))):
                self.action_button_handled = True
                self._toggle_offline_recording(now)
            return
        self.action_button_down_since_ms = None
        self.action_button_handled = False

    def _stream_online(self):
        if not self.wifi.is_connected() or self.control_transport is None:
            return False
        if hasattr(self.control_transport, "is_connected") and not self.control_transport.is_connected():
            return False
        return True

    def _offline_recording_active(self):
        recorder = getattr(self, "offline_recorder", None)
        return recorder is not None and bool(recorder.active)

    def _offline_recording_eligible(self, now=None):
        if self.wifi.setup_active() or self._in_maintenance() or self.reboot_required:
            return False
        if not self.wifi.is_connected():
            return True
        if self.control_transport is None or not self.control_transport.is_connected():
            now = time.ticks_ms() if now is None else now
            if not self.control_disconnected_since_ms:
                return False
            return time.ticks_diff(now, self.control_disconnected_since_ms) >= int(getattr(config, "OFFLINE_CONTROL_OFFLINE_MS", 10000))
        return False

    def _ensure_offline_recorder(self):
        if self.offline_recorder is not None:
            return self.offline_recorder
        from offline_recorder import OfflineRecorder
        self.offline_recorder = OfflineRecorder(getattr(config, "OFFLINE_RECORD_DIR", "data/offline"), self.logger)
        return self.offline_recorder

    def _service_offline_state(self, now):
        connected = self._stream_online()
        if connected:
            self.control_disconnected_since_ms = 0
            if not self.control_connected_since_ms:
                self.control_connected_since_ms = now
        else:
            self.control_connected_since_ms = 0
            if not self.control_disconnected_since_ms:
                self.control_disconnected_since_ms = now

        if self.offline_recorder is not None:
            self.offline_recorder.set_eligible(self._offline_recording_eligible(now))
            if (self.offline_recorder.active
                    and connected
                    and self.control_connected_since_ms
                    and time.ticks_diff(now, self.control_connected_since_ms) >= int(getattr(config, "OFFLINE_RECONNECT_STABLE_MS", 10000))):
                self.offline_recorder.stop("server_reconnected")
                self.update_led_state()

        if self.offline_led_unavailable_until_ms and time.ticks_diff(now, self.offline_led_unavailable_until_ms) >= 0:
            self.offline_led_unavailable_until_ms = 0
            self.update_led_state()

    def _toggle_offline_recording(self, now):
        recorder = self._ensure_offline_recorder()
        if recorder.active:
            recorder.stop("button_stop")
            self.update_led_state()
            return
        if not self._offline_recording_eligible(now):
            self.offline_led_unavailable_until_ms = time.ticks_add(now, 1200) if hasattr(time, "ticks_add") else now + 1200
            self.update_led_state()
            return
        if not self._prepare_offline_scan():
            self.offline_led_unavailable_until_ms = time.ticks_add(now, 1200) if hasattr(time, "ticks_add") else now + 1200
            self.update_led_state()
            return
        ok, message = recorder.begin(now)
        recorder.set_eligible(True)
        if not ok:
            self.logger.warn("offline_recording_start_failed {}".format(message))
            self.offline_led_unavailable_until_ms = time.ticks_add(now, 1200) if hasattr(time, "ticks_add") else now + 1200
        self.update_led_state()

    def _prepare_offline_scan(self):
        if self.scan_ready and self.hardware_ready:
            return True
        if not self._matrix_configured():
            self.logger.warn("offline_recording_no_layout")
            return False
        try:
            self._ensure_streaming_services()
            self._ensure_runtime_hardware()
            return bool(self.scan_ready)
        except Exception as exc:
            self.logger.warn("offline_recording_scan_prepare_failed {}".format(exc))
            return False

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

    def _run_findme(self, reason="manual"):
        if not self.wifi.is_connected():
            return {"ok": False, "error": "wifi_not_connected"}
        gc.collect()
        self.last_findme_ms = time.ticks_ms()
        result = self.wifi.run_findme(reason=reason)
        self.runtime = self.config_store.load_runtime()
        if result.get("ok"):
            if self.control_transport is not None:
                self.control_transport.reconfigure()
            if self.udp_stream is not None:
                self.udp_stream.reconfigure()
        else:
            gc.collect()
        self.update_led_state()
        return result

    def _handle_findme_switch_gateway(self, request):
        preferred_gateway_id = str(request.get("preferred_gateway_id") or request.get("gateway_id") or "").strip()
        claim_id = str(request.get("claim_id") or "").strip()
        ttl_ms = int(request.get("ttl_ms") or 30000)
        if not preferred_gateway_id:
            return self._ok("findme_switch_failed", error="preferred_gateway_id_required", applied=False)
        if not claim_id:
            return self._ok("findme_switch_failed", error="claim_id_required", applied=False)
        now = time.ticks_ms()
        expires_at = time.ticks_add(now, ttl_ms) if hasattr(time, "ticks_add") else now + ttl_ms
        self.runtime = self.config_store.update_runtime({
            "server": {
                "host": "",
                "tcp_port": int(getattr(config, "DEFAULT_TCP_CONTROL_PORT", 22345)),
                "udp_port": int(getattr(config, "DEFAULT_UDP_STREAM_PORT", 13250)),
                "source": "findme",
                "gateway_id": "",
            },
            "findme": {
                "state": "switching",
                "gateway_id": "",
                "gateway_name": "",
                "host": "",
                "tcp_port": int(getattr(config, "DEFAULT_TCP_CONTROL_PORT", 22345)),
                "udp_port": int(getattr(config, "DEFAULT_UDP_STREAM_PORT", 13250)),
                "last_error": "",
                "preferred_gateway_id": preferred_gateway_id,
                "claim_id": claim_id,
                "claim_expires_at_ms": int(expires_at),
                "last_claim_error": "",
            },
            "transport": {"mode": "udp_tcp"},
        })
        if self.control_transport is not None:
            self.control_transport.close()
        if self.udp_stream is not None:
            self.udp_stream.reconfigure()
        result = self._run_findme("claim")
        if result.get("ok"):
            return self._ok("findme_switch_complete", applied=True, findme=result)
        return self._ok("findme_switch_started", applied=True, findme=result, error=result.get("error", "findme_no_gateway"))

    def _service_findme(self, now):
        if not self.wifi.is_connected() or self.wifi.setup_active():
            return False
        if self.control_transport is not None and self.control_transport.is_connected():
            return False
        state = self._findme_status()
        server = (self.runtime or {}).get("server", {}) or {}
        host = str(server.get("host") or state.get("host") or "").strip()
        should_rediscover = (
            not host
            or state.get("state") in ("rejected", "switching", "no_gateway")
            or state.get("last_error") in ("device_rejected", "findme_no_gateway")
        )
        long_attach_failure_ms = int(getattr(config, "GATEWAY_ATTACH_REDISCOVER_MS", 60000))
        if not should_rediscover and time.ticks_diff(now, self.last_findme_ms) < long_attach_failure_ms:
            return False
        retry_ms = int(getattr(config, "GATEWAY_DISCOVERY_RETRY_MS", 5000))
        if time.ticks_diff(now, self.last_findme_ms) < retry_ms:
            return False
        self._run_findme("retry")
        return True

    def _handle_findme_event(self, event):
        if not isinstance(event, dict):
            return
        runtime = self.config_store.load_runtime()
        state = dict(runtime.get("findme", {}) or {})
        event_type = event.get("type")
        gateway_id = str(event.get("gateway_id") or state.get("gateway_id", ""))
        if event_type == "nh_findme_accept":
            state.update({
                "state": "attached",
                "gateway_id": gateway_id,
                "session_id": str(event.get("session_id") or ""),
                "last_error": "",
                "attached_at_ms": time.ticks_ms(),
            })
            self.runtime = self.config_store.update_runtime({"findme": state})
            return
        if event_type == "nh_findme_reject":
            rejected = list(state.get("rejected_gateways", []) or [])
            rejected.append({
                "gateway_id": gateway_id,
                "reason": str(event.get("reason") or "device_rejected"),
                "cooldown_ms": int(event.get("cooldown_ms") or 30000),
                "rejected_at_ms": time.ticks_ms(),
            })
            state.update({
                "state": "rejected",
                "gateway_id": gateway_id,
                "last_error": str(event.get("reason") or "device_rejected"),
                "rejected_gateways": rejected[-6:],
            })
            self.runtime = self.config_store.update_runtime({
                "findme": state,
                "server": {
                    "host": "",
                    "tcp_port": int(getattr(config, "DEFAULT_TCP_CONTROL_PORT", 22345)),
                    "udp_port": int(getattr(config, "DEFAULT_UDP_STREAM_PORT", 13250)),
                    "source": "findme",
                    "gateway_id": "",
                },
            })
            self.last_findme_ms = 0
            self.update_led_state()

    def _reboot_due(self, now):
        if self.reboot_deadline_ms is None:
            return True
        return time.ticks_diff(now, self.reboot_deadline_ms) >= 0

    def _announce_status(self, now=None, force=False):
        if self.control_transport is None:
            return False
        if not self.wifi.is_connected():
            return False
        if now is None:
            now = time.ticks_ms()
        if (not force
                and time.ticks_diff(now, self.last_status_announce_ms) < config.STATUS_ANNOUNCE_INTERVAL_MS):
            return False
        payload = self._status_announce_payload()
        payload["message"] = "status_announce"
        ok = self.control_transport.publish_status(payload, self.wifi.is_connected())
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
        return bool(addr and addr[0] == "tcp")

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

        if cmd == "memory_status":
            return self._memory_status_response()

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

        if cmd in self._calibration_service_commands():
            if not self._in_maintenance():
                return self._ok("maintenance_required", error="maintenance_required")
            self._ensure_calibration_services()
        elif cmd in self._file_service_commands():
            if not self._in_maintenance():
                return self._ok("maintenance_required", error="maintenance_required")
            self._ensure_file_services()
        elif cmd in self._maintenance_service_commands():
            if not self._in_maintenance():
                return self._ok("maintenance_required", error="maintenance_required")

        if cmd == "check_recovery_release":
            return self._check_recovery_release(request)

        if cmd == "write_recovery":
            return self._write_recovery(request)

        if cmd == "reload_config":
            self._apply_runtime_reload()
            return self._ok("config_reloaded", applied=True)

        if cmd == "findme_discover":
            result = self._run_findme("command")
            if result.get("ok"):
                return self._ok("findme_discovered", applied=True, findme=result)
            return self._ok("findme_failed", error=result.get("error", "findme_no_gateway"), findme=result)

        if cmd == "findme_switch_gateway":
            return self._handle_findme_switch_gateway(request)

        if cmd == "set_transport":
            runtime = self.config_store.update_runtime({
                "transport": {
                    "mode": "udp_tcp",
                },
            })
            self.runtime = runtime
            return self._ok("transport_updated", applied=True)

        if cmd == "set_logging":
            return self._set_logging(request)

        if cmd == "set_scan_timing":
            return self._set_scan_timing(request)

        if cmd == "set_indicators":
            return self._set_indicators(request)

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
            old_layout = (self.runtime or {}).get("matrix_layout", {}) or {}
            rollback_layout = {
                "active_rows": list(old_layout.get("active_rows", [])),
                "active_cols": list(old_layout.get("active_cols", [])),
            }
            self.runtime = self.config_store.update_runtime({
                "matrix_layout_state": {
                    "pending": True,
                    "committed": False,
                    "pending_rows": rows,
                    "pending_cols": cols,
                    "last_error": "",
                },
                "matrix_scan_state": {
                    "active": False,
                    "autostart_disabled": False,
                    "last_error": "",
                },
            })
            try:
                self._apply_matrix_layout_candidate(rows, cols)
                if self.hardware_ready and not self._in_maintenance():
                    ok, error = self._probe_scan_health()
                    if not ok:
                        raise RuntimeError(error or "scan_health_probe_failed")
            except Exception as exc:
                self._stop_scan()
                rollback_runtime = self.config_store.update_runtime({
                    "matrix_layout": rollback_layout,
                    "matrix_layout_state": {
                        "pending": False,
                        "committed": False,
                        "last_error": str(exc),
                    },
                    "matrix_scan_state": {
                        "active": False,
                        "autostart_disabled": True,
                        "last_error": str(exc),
                    },
                })
                self.runtime = rollback_runtime
                try:
                    self._apply_matrix_layout()
                except Exception as rollback_exc:
                    self._stop_scan()
                    self.logger.warn("matrix_layout_rollback_scan_failed {}".format(rollback_exc))
                return {
                    "status": "error",
                    "message": "matrix_layout_failed",
                    "runtime": rollback_runtime,
                    "reboot_required": False,
                    "applied": False,
                    "error": str(exc),
                    "scan_memory": self._native_memory_stats(),
                }
            runtime = self.config_store.update_runtime({
                "matrix_layout": {
                    "active_rows": rows,
                    "active_cols": cols,
                },
                "matrix_layout_state": {
                    "pending": False,
                    "committed": True,
                    "last_error": "",
                },
                "matrix_scan_state": {
                    "active": bool(self.scan_ready),
                    "autostart_disabled": False,
                    "last_error": "",
                },
            })
            self.runtime = runtime
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
            self.wifi.start_setup_portal("tcp_command")
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
                "error": "" if result.get("ok") else ("findme_no_gateway" if result.get("wifi_connected") else "wifi_connect_failed"),
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
        self.latest_imu = None
        self.latest_battery = None
        gc.collect()
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
            self._start_scan_if_configured(force=True, persist_state=False)
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

    def _maintenance_scan_preflight(self, sensor_count):
        gc.collect()
        try:
            heap_free = int(gc.mem_free())
        except Exception:
            heap_free = 0
        native = self._native_memory_stats()
        try:
            largest = int(native.get("heap_largest_free_block", 0) or 0)
        except Exception:
            largest = 0
        min_free = int(getattr(config, "CALIBRATION_MIN_HEAP_FREE", getattr(config, "SCAN_MIN_HEAP_FREE", 16384)))
        min_largest = int(getattr(config, "CALIBRATION_MIN_LARGEST_FREE_BLOCK", getattr(config, "SCAN_MIN_LARGEST_FREE_BLOCK", 8192)))
        if heap_free and heap_free < min_free:
            return {
                "status": "error",
                "message": "calibration_heap_low",
                "error": "calibration_heap_low",
                "heap_free": heap_free,
                "min_heap_free": min_free,
                "cells": sensor_count,
                "scan_memory": native,
                "reboot_required": False,
                "applied": False,
            }
        if largest and largest < min_largest:
            return {
                "status": "error",
                "message": "calibration_largest_block_low",
                "error": "calibration_largest_block_low",
                "heap_largest_free_block": largest,
                "min_largest_free_block": min_largest,
                "cells": sensor_count,
                "scan_memory": native,
                "reboot_required": False,
                "applied": False,
            }
        return None

    def _frame_u16(self, view, offset):
        return int(view[offset]) | (int(view[offset + 1]) << 8)

    def _scan_frame_payload_view(self, frame_view, sensor_count):
        view = memoryview(frame_view)
        header_size = 16
        payload_type_mv_u16 = 1
        expected_len = header_size + (sensor_count * 2)
        if len(view) < expected_len:
            raise ValueError("scan_frame_short")
        point_count = self._frame_u16(view, 12)
        payload_type = self._frame_u16(view, 14)
        if payload_type != payload_type_mv_u16:
            raise ValueError("scan_frame_payload_type")
        if point_count < sensor_count:
            raise ValueError("scan_frame_point_count")
        return view, header_size

    def _maintenance_sample_all(self, request):
        if not self._in_maintenance():
            return self._ok("maintenance_required", error="maintenance_required")
        if not self._matrix_configured():
            return self._ok("matrix_layout_required", error="matrix_layout_required")
        level = float(request["level"])
        duration_ms = int(request.get("duration_ms", 3000))
        active_rows = self._active_rows()
        active_cols = self._active_cols()
        sensor_count = len(active_rows) * len(active_cols)
        preflight = self._maintenance_scan_preflight(sensor_count)
        if preflight is not None:
            return preflight
        sums = None
        counts = None
        try:
            self._start_scan_if_configured(force=True, persist_state=False)
            end_ms = time.ticks_add(time.ticks_ms(), max(1, duration_ms))
            sums = [0.0] * sensor_count
            counts = [0] * sensor_count
            while time.ticks_diff(end_ms, time.ticks_ms()) > 0:
                self.vdboard.scan.service()
                frame_view = self.vdboard.scan.pop_frame_mv()
                if frame_view is None:
                    time.sleep_ms(1)
                    continue
                payload_view, payload_offset = self._scan_frame_payload_view(frame_view, sensor_count)
                for idx in range(sensor_count):
                    offset = payload_offset + (idx * 2)
                    value = self._frame_u16(payload_view, offset)
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
            sums = None
            counts = None
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

    def _memory_status_response(self):
        return {
            "status": "ok",
            "message": "memory_status",
            "command": "memory_status",
            "mode": self.mode,
            "device_id": self.device_uid,
            "device_uid": self.device_uid,
            "device_name": self.device_name,
            "memory": self._memory_status(),
            "findme": {
                "connected": bool(self.control_transport is not None and self.control_transport.is_connected()),
                "state": self._findme_status().get("state", ""),
                "gateway_id": self._findme_status().get("gateway_id", ""),
            },
            "runtime_version": getattr(config, "RUNTIME_VERSION", "unknown"),
            "os_version": getattr(config, "FIRMWARE_VERSION", "unknown"),
            "recovery_version": self._recovery_version(),
            "reboot_required": False,
            "applied": False,
        }

    def _log_memory_stage(self, stage):
        try:
            free = int(gc.mem_free())
        except Exception:
            free = 0
        try:
            allocated = int(gc.mem_alloc())
        except Exception:
            allocated = 0
        self.logger.info("mem_stage stage={} free={} allocated={}".format(stage, free, allocated))

    def _wifi_setup_status_light(self):
        if self.wifi.setup_active():
            return self.wifi.portal_status(include_storage=True)
        return {
            "active": False,
            "state": getattr(self.wifi, "state", ""),
            "last_error": getattr(self.wifi, "last_error", ""),
            "last_setup_result": getattr(self.wifi, "last_setup_result", ""),
        }

    def _hello_status(self):
        runtime = self.runtime if isinstance(self.runtime, dict) else {}
        return {
            "status": "ok",
            "message": "hello",
            "mode": self.mode,
            "device_id": self.device_uid,
            "device_uid": self.device_uid,
            "device_name": self.device_name,
            "system": self._system_status(),
            "runtime_version": getattr(config, "RUNTIME_VERSION", "unknown"),
            "os_version": getattr(config, "FIRMWARE_VERSION", "unknown"),
            "recovery_version": self._recovery_version(),
            "runtime": {
                "mode": self.mode,
                "transport": runtime.get("transport", {}),
                "findme": self._findme_status(),
                "matrix_layout": {
                    "active_rows": self._active_rows(),
                    "active_cols": self._active_cols(),
                },
                "matrix_layout_state": runtime.get("matrix_layout_state", {}),
                "matrix_scan_state": runtime.get("matrix_scan_state", {}),
                "scan_timing": runtime.get("scan_timing", {}),
            },
            "findme": self._findme_status(),
            "wifi_connected": self.wifi.is_connected(),
        }

    def _file_call(self, fn, *args):
        try:
            return fn(*args)
        except ValueError as exc:
            return self._ok(str(exc), error=str(exc))

    def _status(self):
        recorder_status = self._offline_recording_status()
        return {
            "status": "ok",
            "message": "status",
            "mode": self.mode,
            "maintenance_reason": self.maintenance_reason,
            "reboot_required": self.reboot_required,
            "applied": False,
            "device_id": self.device_uid,
            "device_uid": self.device_uid,
            "device_name": self.device_name,
            "system": self._system_status(),
            "memory": self._memory_status(),
            "wifi_state": self.wifi.state,
            "wifi_setup": self._wifi_setup_status_light(),
            "ntp": self.time_sync.status() if self.time_sync is not None else {},
            "filter": self.config_store.load_filter(),
            "runtime": self.config_store.load_runtime(),
            "findme": self._findme_status(),
            "logging": self._logging_status(),
            "battery": self._battery_status(refresh=False),
            "indicators": self._indicator_status(),
            "update_state": self._current_update_state(),
            "calibration_levels": self.calibration.list_levels() if self.calibration is not None else [],
            "available_rows": list(config.AVAILABLE_ROWS),
            "available_cols": list(config.AVAILABLE_COLS),
            "active_rows": self._active_rows(),
            "active_cols": self._active_cols(),
            "matrix_configured": self._matrix_configured(),
            "matrix_shape": {"rows": len(self._active_rows()), "cols": len(self._active_cols())},
            "last_matrix_start_failed": self.last_matrix_start_failed,
            "last_scan_health": self.last_scan_health,
            "sent_packets": self.sent_packets,
            "failed_sends": self.failed_sends,
            "scan": self.vdboard.scan.stats() if self.vdboard is not None else (),
            "stream": self.vdboard.scan.stream_stats() if (
                self.vdboard is not None and hasattr(self.vdboard.scan, "stream_stats")
            ) else {},
            "offline_recording": recorder_status,
        }

    def _status_announce_payload(self):
        runtime = self.runtime if isinstance(self.runtime, dict) else {}
        recorder_status = self._offline_recording_status()
        return {
            "status": "ok",
            "message": "status_announce",
            "mode": self.mode,
            "reboot_required": self.reboot_required,
            "device_id": self.device_uid,
            "device_uid": self.device_uid,
            "device_name": self.device_name,
            "system": self._system_status(),
            "runtime": {
                "mode": self.mode,
                "transport": runtime.get("transport", {}),
                "findme": self._findme_status(),
                "logging": runtime.get("logging", {}),
                "scan_timing": runtime.get("scan_timing", {}),
                "matrix_layout": {
                    "active_rows": self._active_rows(),
                    "active_cols": self._active_cols(),
                },
                "matrix_layout_state": runtime.get("matrix_layout_state", {}),
                "matrix_scan_state": runtime.get("matrix_scan_state", {}),
                "system": runtime.get("system", {}),
            },
            "wifi_state": self.wifi.state,
            "wifi_connected": self.wifi.is_connected(),
            "findme": self._findme_status(),
            "logging": self._logging_status(),
            "battery": self._battery_status(refresh=False),
            "matrix_configured": self._matrix_configured(),
            "matrix_shape": {"rows": len(self._active_rows()), "cols": len(self._active_cols())},
            "last_matrix_start_failed": self.last_matrix_start_failed,
            "update_state": self._current_update_state(),
            "sent_packets": self.sent_packets,
            "failed_sends": self.failed_sends,
            "offline_recording": recorder_status,
        }

    def _maintenance_status(self):
        status = self._status()
        status["message"] = "maintenance_status"
        status["scan_stopped"] = not self.scan_ready
        return status

    def _offline_recording_status(self):
        if self.offline_recorder is None:
            return {
                "eligible": self._offline_recording_eligible(),
                "active": False,
                "rolling": False,
                "bytes_used": 0,
                "bytes_limit": 0,
                "estimated_seconds_until_rollover": 0,
                "segment_count": 0,
                "dropped_frames": 0,
                "stop_reason": "",
                "error": "",
            }
        self.offline_recorder.set_eligible(self._offline_recording_eligible())
        return self.offline_recorder.status()

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
            "set_scan_timing",
            "set_indicators",
            "log_tail",
            "log_read_tail",
            "log_clear",
        )

    def _maintenance_service_commands(self):
        return self._calibration_service_commands() + self._file_service_commands() + (
            "log_tail",
            "log_read_tail",
            "log_clear",
        )

    def _calibration_service_commands(self):
        return (
            "calibration_sample_cell",
            "calibration_sample_all",
            "calibration_save",
            "dump_calibration",
            "delete_calibration_level",
        )

    def _file_service_commands(self):
        return (
            "file_list",
            "file_upload_begin",
            "file_upload_chunk",
            "file_upload_finish",
            "file_download_begin",
            "file_download_chunk",
            "file_delete",
            "fs_list",
            "fs_read",
        )

    def _print_setup(self):
        self.logger.info(
            "boot mode=normal device={} id={} version={} matrix={}x{} active={}x{} wifi_setup={}".format(
                self.device_name,
                self.device_uid,
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
            raise RuntimeError("native_stream_required")

    def _ensure_runtime_hardware(self):
        if self.hardware_ready:
            return
        self._ensure_streaming_services()

        self._start_scan_if_configured()

        if self.battery:
            self.logger.info("setup_battery_begin_start")
            self.battery.begin()
            self.logger.info("setup_battery_begin_done")
            self._log_memory_stage("battery")

        if self.imu:
            self.logger.info("setup_imu_begin_start")
            self.imu.begin()
            self.logger.info("setup_imu_begin_done")
            self._log_memory_stage("imu")

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

    def _has_pending_matrix_layout(self):
        state = (self.runtime or {}).get("matrix_layout_state", {}) or {}
        return bool(state.get("pending"))

    def _has_committed_matrix_layout(self):
        state = (self.runtime or {}).get("matrix_layout_state", {}) or {}
        return bool(state.get("committed"))

    def _has_interrupted_scan_session(self):
        state = (self.runtime or {}).get("matrix_scan_state", {}) or {}
        return bool(state.get("active"))

    def _matrix_autostart_disabled(self):
        state = (self.runtime or {}).get("matrix_scan_state", {}) or {}
        return bool(state.get("autostart_disabled"))

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

    def _set_scan_timing(self, request):
        old_timing = dict((self.runtime or {}).get("scan_timing", {}) or {})
        new_timing = dict(old_timing)
        if "target_fps" in request:
            try:
                target_fps = int(request.get("target_fps"))
            except Exception:
                return self._ok("scan_timing_invalid", error="target_fps_must_be_positive")
            if target_fps <= 0:
                return self._ok("scan_timing_invalid", error="target_fps_must_be_positive")
            if target_fps > 65535:
                return self._ok("scan_timing_invalid", error="target_fps_too_large")
            new_timing["target_fps"] = target_fps
        if "settle_us" in request:
            try:
                settle_us = int(request.get("settle_us"))
            except Exception:
                return self._ok("scan_timing_invalid", error="settle_us_must_be_non_negative")
            if settle_us < 0:
                return self._ok("scan_timing_invalid", error="settle_us_must_be_non_negative")
            if settle_us > 65535:
                return self._ok("scan_timing_invalid", error="settle_us_too_large")
            new_timing["settle_us"] = settle_us
        if new_timing == old_timing:
            return {
                "status": "ok",
                "message": "scan_timing_updated",
                "runtime": self.runtime,
                "reboot_required": False,
                "applied": True,
            }
        scan_was_ready = bool(self.scan_ready)

        runtime = self.config_store.update_runtime({"scan_timing": new_timing})
        self.runtime = runtime
        try:
            if scan_was_ready and self.hardware_ready and not self._in_maintenance():
                self._apply_matrix_layout(force=True)
                ok, error = self._probe_scan_health()
                if not ok:
                    raise RuntimeError(error or "scan_health_probe_failed")
        except Exception as exc:
            rollback_runtime = self.config_store.update_runtime({"scan_timing": old_timing})
            self.runtime = rollback_runtime
            try:
                if scan_was_ready and self.hardware_ready and not self._in_maintenance():
                    self._apply_matrix_layout(force=True)
                else:
                    self._stop_scan()
            except Exception as rollback_exc:
                self._stop_scan()
                self.logger.warn("scan_timing_rollback_failed {}".format(rollback_exc))
            return {
                "status": "error",
                "message": "scan_timing_failed",
                "runtime": rollback_runtime,
                "reboot_required": False,
                "applied": False,
                "error": str(exc),
                "scan_memory": self._native_memory_stats(),
            }

        return {
            "status": "ok",
            "message": "scan_timing_updated",
            "runtime": runtime,
            "reboot_required": False,
            "applied": True,
        }

    def _set_indicators(self, request):
        runtime = self.runtime if isinstance(self.runtime, dict) else {}
        current = runtime.get("indicators", {}) or {}
        current_external = dict(current.get("external_led", {}) or {})
        current_oled = dict(current.get("oled", {}) or {})

        external_request = request.get("external_led", {})
        if not isinstance(external_request, dict):
            external_request = {}
        oled_request = request.get("oled", {})
        if not isinstance(oled_request, dict):
            oled_request = {}

        if "external_led_enabled" in request:
            external_request["enabled"] = request.get("external_led_enabled")
        if "external_led_mode" in request:
            external_request["mode"] = request.get("external_led_mode")
        if "manual_preset" in request:
            external_request["manual_preset"] = request.get("manual_preset")
        if "external_led_brightness" in request:
            external_request["brightness"] = request.get("external_led_brightness")
        if "oled_enabled" in request:
            oled_request["enabled"] = request.get("oled_enabled")
        if "oled_page" in request:
            oled_request["page"] = request.get("oled_page")
        if "oled_update_hz" in request:
            oled_request["update_hz"] = request.get("oled_update_hz")
        if "oled_contrast" in request:
            oled_request["contrast"] = request.get("oled_contrast")

        if external_request:
            if "enabled" in external_request:
                current_external["enabled"] = bool(external_request.get("enabled"))
            mode = external_request.get("mode")
            if mode is not None:
                if mode not in ("auto", "manual"):
                    return self._ok("indicators_invalid", error="external_led_mode_invalid")
                current_external["mode"] = mode
            preset = external_request.get("manual_preset")
            if preset is not None:
                if preset not in ("stream_health", "pressure_activity", "recording_focus", "calibration_focus"):
                    return self._ok("indicators_invalid", error="external_led_preset_invalid")
                current_external["manual_preset"] = preset
            if "brightness" in external_request:
                try:
                    brightness = float(external_request.get("brightness"))
                except Exception:
                    return self._ok("indicators_invalid", error="external_led_brightness_invalid")
                current_external["brightness"] = max(0.10, min(0.50, brightness))

        oled_requested = bool(oled_request)
        if oled_requested:
            if "enabled" in oled_request:
                current_oled["enabled"] = bool(oled_request.get("enabled"))
            page = oled_request.get("page")
            if page is not None:
                if page not in ("live_status", "sensor_snapshot", "recording_status"):
                    return self._ok("indicators_invalid", error="oled_page_invalid")
                current_oled["page"] = page
            if "update_hz" in oled_request:
                try:
                    current_oled["update_hz"] = max(1, min(5, int(oled_request.get("update_hz"))))
                except Exception:
                    return self._ok("indicators_invalid", error="oled_update_hz_invalid")
            if "contrast" in oled_request:
                try:
                    current_oled["contrast"] = max(0, min(255, int(oled_request.get("contrast"))))
                except Exception:
                    return self._ok("indicators_invalid", error="oled_contrast_invalid")

        self._ensure_led()
        if self.led:
            # Probe optional OLED only when the requested config would actively use it.
            oled_wants_enabled = bool(current_oled.get("enabled", True))
            if oled_requested and oled_wants_enabled:
                if not hasattr(self.led, "detect_oled") or not self.led.detect_oled():
                    return self._ok("oled_not_detected", error="oled_not_detected")

        indicators = {
            "external_led": current_external,
            "oled": current_oled,
        }
        runtime = self.config_store.update_runtime({"indicators": indicators})
        self.runtime = runtime
        if self.led:
            self.led.configure(indicators)
            if hasattr(self.led, "set_context"):
                self.led.set_context(self._indicator_context())
        return {
            "status": "ok",
            "message": "indicators_updated",
            "runtime": runtime,
            "indicators": self._indicator_status(),
            "reboot_required": False,
            "applied": True,
        }

    def _rebuild_matrix_pipeline(self):
        active_rows = self._active_rows()
        active_cols = self._active_cols()
        if self._native_stream_available():
            self.packet = None
            self.filter_chain = None
            self.tx_buffer = None
            self.native_streaming = True
            return
        raise RuntimeError("native_stream_required")

    def _start_scan_if_configured(self, force=False, persist_state=True):
        if not self._matrix_configured():
            self.scan_ready = False
            self.logger.info("setup_scan_skipped_no_layout")
            return
        if not force and not self._has_committed_matrix_layout():
            self.scan_ready = False
            self.last_matrix_start_failed = True
            self.logger.warn("setup_scan_skipped_uncommitted_layout")
            try:
                self.config_store.update_runtime(
                    {
                        "matrix_layout_state": {
                            "pending": False,
                            "committed": False,
                            "last_error": "layout_requires_reapply",
                        },
                        "matrix_scan_state": {
                            "active": False,
                            "autostart_disabled": True,
                            "last_error": "layout_requires_reapply",
                        },
                    }
                )
            except Exception as exc:
                self.logger.warn("matrix_layout_state_reapply_mark_failed {}".format(exc))
            return
        if not force and self._matrix_autostart_disabled():
            self.scan_ready = False
            self.last_matrix_start_failed = True
            self.logger.warn("setup_scan_autostart_disabled")
            return
        self.logger.info("setup_scan_backend_init_start")
        self._init_scan_backend()
        self.logger.info("setup_scan_backend_init_done")
        self.logger.info("setup_scan_start_start")
        self.vdboard.scan.start()
        self.logger.info("setup_scan_start_done")
        self.scan_ready = True
        if persist_state:
            self._mark_scan_started()

    def _stop_scan(self, persist_state=True):
        if self._offline_recording_active():
            try:
                self.offline_recorder.stop("scan_stopped")
            except Exception as exc:
                self.logger.warn("offline_recording_stop_failed {}".format(exc))
        if self.vdboard is not None:
            try:
                self.vdboard.scan.stop()
            except Exception as exc:
                self.logger.warn("scan_stop_failed {}".format(exc))
        self.scan_ready = False
        self.latest_matrix = None
        self.latest_frame = None
        self.scan_packet_buffer = None
        self._clear_tx_buffer()
        if persist_state:
            self._mark_scan_stopped()

    def _apply_matrix_layout(self, force=False, persist_state=True):
        self._stop_scan(persist_state=persist_state)
        if self.hardware_ready and not self._in_maintenance():
            self._start_scan_if_configured(force=force, persist_state=persist_state)

    def _apply_matrix_layout_candidate(self, rows, cols):
        old_runtime = self.runtime
        candidate = dict(old_runtime or {})
        candidate["matrix_layout"] = {
            "active_rows": list(rows),
            "active_cols": list(cols),
        }
        self.runtime = candidate
        try:
            self._apply_matrix_layout(force=True, persist_state=False)
        except Exception:
            self.runtime = old_runtime
            raise

    def _mark_scan_started(self):
        try:
            self.runtime = self.config_store.update_runtime({
                "matrix_scan_state": {
                    "active": True,
                    "autostart_disabled": False,
                    "last_error": "",
                },
            })
        except Exception as exc:
            self.logger.warn("matrix_scan_state_start_failed {}".format(exc))

    def _mark_scan_stopped(self):
        try:
            state = (self.runtime or {}).get("matrix_scan_state", {}) or {}
            if state.get("active"):
                self.runtime = self.config_store.update_runtime({
                    "matrix_scan_state": {
                        "active": False,
                        "last_error": state.get("last_error", ""),
                    },
                })
        except Exception as exc:
            self.logger.warn("matrix_scan_state_stop_failed {}".format(exc))

    def _scan_stats_tuple(self):
        if self.vdboard is None or not hasattr(self.vdboard.scan, "stats"):
            return ()
        try:
            return self.vdboard.scan.stats()
        except Exception:
            return ()

    def _native_memory_stats(self):
        if self.vdboard is None or not hasattr(self.vdboard.scan, "memory_stats"):
            return {}
        try:
            return self.vdboard.scan.memory_stats()
        except Exception:
            return {}

    def _probe_scan_health(self):
        if not self.scan_ready:
            return False, "scan_not_started"
        if not self._native_stream_available():
            return False, "native_stream_required"
        start_stats = self._scan_stats_tuple()
        start_frames = int(start_stats[0]) if len(start_stats) > 0 else 0
        deadline = time.ticks_add(time.ticks_ms(), int(getattr(config, "SCAN_HEALTH_PROBE_MS", 2000)))
        while time.ticks_diff(deadline, time.ticks_ms()) > 0:
            try:
                self.vdboard.scan.service()
            except Exception as exc:
                return False, "scan_service_failed {}".format(exc)
            stats = self._scan_stats_tuple()
            produced = int(stats[0]) if len(stats) > 0 else 0
            started = bool(int(stats[7])) if len(stats) > 7 else True
            if started and produced > start_frames:
                packet = self._native_pop_packet()
                if packet is None:
                    time.sleep_ms(50)
                    continue
                mem = self._native_memory_stats()
                heap_free = int(mem.get("heap_free", 0) or 0)
                largest = int(mem.get("heap_largest_free_block", 0) or 0)
                min_free = int(getattr(config, "SCAN_MIN_HEAP_FREE", 16384))
                min_largest = int(getattr(config, "SCAN_MIN_LARGEST_FREE_BLOCK", 8192))
                if heap_free and heap_free < min_free:
                    return False, "scan_heap_low free={} min={}".format(heap_free, min_free)
                if largest and largest < min_largest:
                    return False, "scan_largest_block_low largest={} min={}".format(largest, min_largest)
                self.last_scan_health = {
                    "produced_frames": produced,
                    "heap_free": heap_free,
                    "heap_largest_free_block": largest,
                }
                return True, ""
            time.sleep_ms(50)
        return False, "scan_no_frames"

    def _native_packet_buffer_size(self):
        stats = self._native_memory_stats()
        size = int(stats.get("packet_scratch_bytes", 0) or 0)
        return size if size > 0 else 1536

    def _native_pop_packet(self):
        scan = self.vdboard.scan
        if hasattr(scan, "pop_packet_into"):
            if self.scan_packet_buffer is None:
                self.scan_packet_buffer = bytearray(self._native_packet_buffer_size())
            length = scan.pop_packet_into(self.scan_packet_buffer)
            if length is None:
                return None
            length = int(length)
            if length <= 0:
                return None
            return memoryview(self.scan_packet_buffer)[:length]
        self.logger.warn("native_stream_missing_pop_packet_into")
        self._stop_scan()
        return None

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
            and hasattr(self.vdboard.scan, "pop_packet_into")
            and hasattr(self.vdboard.scan, "set_packet_options")
            and hasattr(self.vdboard.scan, "load_calibration")
        )

    def _configure_native_stream(self):
        if not self._native_stream_available():
            self.native_streaming = False
            return False
        try:
            import secrets
            self.vdboard.scan.set_packet_options(
                self.packet_device_uid,
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
        if self.calibration is None and not getattr(config, "KEEP_CALIBRATION_MODULE_LOADED", False):
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
        if self.control_transport is not None and self.udp_stream is not None:
            return
        from tcp_control import TCPControlTransport
        from udp_stream import UDPStreamTransport
        from time_sync import TimeSync
        from utils import RateCounter

        self.control_transport = TCPControlTransport(
            lambda: self.runtime,
            self.device_uid,
            self.logger,
            self._hello_status,
            self._handle_findme_event,
        )
        self.udp_stream = UDPStreamTransport(lambda: self.runtime, self.logger)
        self.time_sync = TimeSync(self.runtime.get("ntp_servers", []))
        self.scan_rate = RateCounter(1000)

        if config.ENABLE_IMU:
            from bmi270 import BMI270
            self.imu = BMI270()
        if config.ENABLE_BATTERY:
            from bq25180 import BQ25180
            self.battery = BQ25180()

    def _ensure_file_services(self):
        if self.filesystem is None:
            from filesystem_api import FilesystemAPI
            self.filesystem = FilesystemAPI(
                getattr(config, "DATA_FILES_DIR", "data/files"),
                getattr(config, "DATA_TMP_DIR", "data/tmp"),
                {
                    "user": getattr(config, "DATA_FILES_DIR", "data/files"),
                    "logs": getattr(config, "DATA_LOG_DIR", "data/logs"),
                    "calibration": getattr(config, "CALIBRATION_DIR", "device_state/calibration"),
                    "offline": getattr(config, "OFFLINE_RECORD_DIR", "data/offline"),
                },
                writable_scopes=("user",),
            )

    def _ensure_calibration_services(self):
        if self.calibration is None:
            from calibration_store import CalibrationStore
            self.calibration = CalibrationStore(config.CALIBRATION_DIR)
            self.calibration.load()

    def _ensure_maintenance_services(self):
        self._ensure_file_services()
        self._ensure_calibration_services()

    def _release_maintenance_services(self):
        self.filesystem = None
        self.calibration = None
        for module_name in ("filesystem_api", "calibration_store", "storage"):
            try:
                import sys
                sys.modules.pop(module_name, None)
            except Exception:
                pass
        gc.collect()

    def _ensure_runtime_services(self):
        self._ensure_streaming_services()
        if not self._native_stream_available():
            raise RuntimeError("native_stream_required")
