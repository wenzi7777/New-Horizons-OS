# sk6812.py
import time
import machine
import neopixel
import gc

import board_pins

try:
    import config as _config
except Exception:
    _config = None


def _cfg(name, default):
    return getattr(_config, name, default) if _config is not None else default


class SK6812Status:
    EXTERNAL_PRESETS = (
        "stream_health",
        "stream_health_idle",
        "pressure_activity",
        "recording_focus",
        "calibration_focus",
    )
    MANUAL_PRESETS = (
        "stream_health",
        "pressure_activity",
        "recording_focus",
        "calibration_focus",
    )
    OLED_PAGES = (
        "live_status",
        "sensor_snapshot",
        "recording_status",
    )

    def __init__(self):
        self.np = None
        self.external_np = None
        self.state = "off"

        self.brightness = 0.08
        self.external_mode = "off"
        self.external_enabled = False
        self.external_manual_preset = "stream_health"
        self.external_active_preset = "off"
        self.external_brightness = self._clamp_brightness(_cfg("EXTERNAL_LED_DEFAULT_BRIGHTNESS", 0.35))
        self.external_last_colors = None
        self.indicator_context = {}

        self.oled_mode = "off"
        self.oled_enabled = False
        self.oled_page = _cfg("OLED_DEFAULT_PAGE", "live_status")
        self.oled_update_hz = int(_cfg("OLED_DEFAULT_UPDATE_HZ", 1))
        self.oled_contrast = int(_cfg("OLED_DEFAULT_CONTRAST", 128))
        self.oled_detected = False
        self.oled_addr = 0
        self.oled_last_error = ""
        self.oled_i2c = None
        self.oled_fb = None
        self.oled_buffer = None
        self.oled_framebuf = None
        self.oled_initialized = False
        self.oled_last_update_ms = 0

        self.last_update_ms = 0
        self.phase = 0
        self.state_map = {
            "boot_window": {
                "intervals": (120, 1380),
                "colors": ((0, 0, 160), (0, 0, 0)),
                "brightness": 0.15,
            },
            "wifi_setup": {
                "intervals": (120, 880),
                "colors": ((0, 180, 180), (0, 0, 0)),
                "brightness": 0.20,
            },
            "findme_no_gateway": {
                "intervals": (90, 90, 90, 720),
                "colors": ((255, 90, 0), (0, 0, 0), (200, 0, 0), (0, 0, 0)),
                "brightness": 0.35,
            },
            "findme_gateway_lost": {
                "intervals": (120, 120, 120, 120, 120, 900),
                "colors": ((255, 160, 0), (0, 0, 0), (255, 60, 0), (0, 0, 0), (120, 0, 0), (0, 0, 0)),
                "brightness": 0.35,
            },
            "findme_rejected": {
                "intervals": (100, 100, 100, 900),
                "colors": ((180, 0, 255), (0, 0, 0), (255, 0, 0), (0, 0, 0)),
                "brightness": 0.35,
            },
            "normal": {
                "intervals": (90, 120, 90, 4700),
                "colors": ((0, 170, 0), (0, 0, 0), (0, 90, 0), (0, 0, 0)),
                "brightness": 0.15,
            },
            "updating": {
                "intervals": (1800, 100, 1800, 100),
                "colors": ((80, 0, 255), (0, 0, 0), (0, 180, 255), (0, 0, 0)),
                "brightness": 0.45,
            },
            "reboot_required": {
                "intervals": (1000, 100),
                "colors": ((120, 0, 255), (0, 0, 0)),
                "brightness": 0.35,
            },
            "maintenance": {
                "intervals": (100, 120, 100, 1680),
                "colors": ((110, 0, 180), (0, 0, 0), (70, 0, 140), (0, 0, 0)),
                "brightness": 0.25,
            },
            "calibration": {
                "intervals": (90, 90, 90, 90, 90, 1050),
                "colors": ((200, 0, 140), (0, 0, 0), (150, 0, 100), (0, 0, 0), (90, 0, 70), (0, 0, 0)),
                "brightness": 0.30,
            },
            "error": {
                "intervals": (70, 70, 70, 70, 70, 1500),
                "colors": ((255, 0, 0), (0, 0, 0), (180, 0, 0), (0, 0, 0), (100, 0, 0), (0, 0, 0)),
                "brightness": 0.50,
            },
            "charging": {
                "intervals": (120, 3880),
                "colors": ((255, 120, 0), (0, 0, 0)),
                "brightness": 0.20,
            },
            "charge_done": {
                "intervals": (120, 5880),
                "colors": ((57, 197, 187), (0, 0, 0)),
                "brightness": 0.20,
            },
            "offline_record_ok": {
                "intervals": (140, 1860),
                "colors": ((120, 0, 255), (0, 0, 0)),
                "brightness": 0.25,
            },
            "offline_record_low": {
                "intervals": (140, 1860),
                "colors": ((255, 200, 0), (0, 0, 0)),
                "brightness": 0.30,
            },
            "offline_record_critical": {
                "intervals": (120, 120, 120, 1640),
                "colors": ((255, 110, 0), (0, 0, 0), (200, 70, 0), (0, 0, 0)),
                "brightness": 0.35,
            },
            "offline_record_urgent": {
                "intervals": (90, 90, 90, 90, 90, 1550),
                "colors": ((255, 40, 0), (0, 0, 0), (220, 70, 0), (0, 0, 0), (180, 0, 0), (0, 0, 0)),
                "brightness": 0.45,
            },
            "offline_record_rolling": {
                "intervals": (120, 120, 120, 1640),
                "colors": ((255, 0, 180), (0, 0, 0), (0, 180, 180), (0, 0, 0)),
                "brightness": 0.35,
            },
            "offline_record_unavailable": {
                "intervals": (80, 80, 80, 760),
                "colors": ((255, 0, 0), (0, 0, 0), (80, 0, 0), (0, 0, 0)),
                "brightness": 0.35,
            },
            "off": {
                "intervals": (1000,),
                "colors": ((0, 0, 0),),
                "brightness": 0.0,
            },
        }

    def begin(self):
        self.np = neopixel.NeoPixel(
            machine.Pin(board_pins.SK6812_PIN),
            board_pins.SK6812_COUNT
        )
        self.off()
        print("SK6812 begin")

    def configure(self, indicators):
        if not isinstance(indicators, dict):
            indicators = {}
        external = indicators.get("external_led", {})
        if isinstance(external, dict):
            mode = external.get("mode")
            if mode in ("off", "on"):
                self.external_mode = mode
                self.external_enabled = mode == "on"
                if mode == "off":
                    self.release_external_led("mode_off")
            preset = external.get("manual_preset")
            if preset in self.MANUAL_PRESETS:
                self.external_manual_preset = preset
            if "brightness" in external:
                self.external_brightness = self._clamp_brightness(external.get("brightness"))

        oled = indicators.get("oled", {})
        if isinstance(oled, dict):
            mode = oled.get("mode")
            if mode in ("off", "auto", "on"):
                self.oled_mode = mode
                self.oled_enabled = mode != "off"
                if mode == "off":
                    self.release_oled("mode_off")
            if oled.get("page") in self.OLED_PAGES:
                self.oled_page = oled.get("page")
            if "update_hz" in oled:
                self.oled_update_hz = max(1, min(5, int(oled.get("update_hz") or 1)))
            if "contrast" in oled:
                self.oled_contrast = max(0, min(255, int(oled.get("contrast") or 0)))
            if self.oled_initialized and self.oled_fb is not None:
                self._oled_cmd(0x81)
                self._oled_cmd(self.oled_contrast)

    def status(self):
        return {
            "external_led": {
                "mode": self.external_mode,
                "active": self.external_np is not None,
                "manual_preset": self.external_manual_preset,
                "active_preset": self.external_active_preset,
                "brightness": self.external_brightness,
            },
            "oled": {
                "mode": self.oled_mode,
                "active": bool(self.oled_initialized and self.oled_fb is not None),
                "detected": self.oled_detected,
                "addr": "0x{:02X}".format(self.oled_addr) if self.oled_addr else "",
                "page": self.oled_page,
                "update_hz": self.oled_update_hz,
                "contrast": self.oled_contrast,
                "last_error": self.oled_last_error,
            },
        }

    def ensure_optional_indicators(self):
        active = False
        if self.external_mode == "on":
            active = self._init_external_led() or active
        else:
            self.release_external_led("mode_off")

        if self.oled_mode == "off":
            self.release_oled("mode_off")
        elif self.detect_oled():
            active = self._init_oled() or active
        else:
            self.release_oled("not_detected")
        return active

    def release_optional_indicators(self, reason="released"):
        self.release_external_led(reason)
        self.release_oled(reason)

    def _init_external_led(self):
        if self.external_np is not None:
            return True
        count = int(getattr(board_pins, "WS2812B_COUNT", 0))
        if count <= 0:
            return False
        try:
            self.external_np = neopixel.NeoPixel(
                machine.Pin(board_pins.WS2812B_PIN),
                count,
            )
            self.external_last_colors = None
            self._write_external(((0, 0, 0),) * count, force=True)
            return True
        except Exception:
            self.external_np = None
            self.external_last_colors = None
            return False

    def release_external_led(self, reason="released"):
        had_resource = self.external_np is not None or self.external_last_colors is not None
        if self.external_np is not None:
            try:
                self._write_external(((0, 0, 0),) * int(getattr(board_pins, "WS2812B_COUNT", 0)), force=True)
            except Exception:
                pass
        self.external_np = None
        self.external_last_colors = None
        self.external_active_preset = "off"
        if had_resource:
            try:
                gc.collect()
            except Exception:
                pass

    def release_oled(self, reason="released"):
        had_resource = bool(self.oled_i2c or self.oled_fb or self.oled_buffer or self.oled_framebuf or self.oled_initialized)
        if self.oled_fb is not None:
            try:
                self._clear_oled()
            except Exception:
                pass
        self.oled_i2c = None
        self.oled_fb = None
        self.oled_buffer = None
        self.oled_framebuf = None
        self.oled_initialized = False
        self.oled_detected = False
        self.oled_addr = 0
        self.oled_last_error = "not_detected" if reason == "not_detected" else ""
        if had_resource:
            try:
                gc.collect()
            except Exception:
                pass

    def set_context(self, context):
        self.indicator_context = context if isinstance(context, dict) else {}
        self.external_active_preset = self._active_external_preset()

    def detect_oled(self):
        if self.oled_detected and self.oled_i2c is not None:
            return True
        try:
            import i2c_bus
            i2c = i2c_bus.get_i2c()
            addresses = i2c.scan()
            for addr in (getattr(board_pins, "SSD1306_ADDR_PRIMARY", 0x3C), getattr(board_pins, "SSD1306_ADDR_FALLBACK", 0x3D)):
                if addr in addresses:
                    self.oled_i2c = i2c
                    self.oled_addr = addr
                    self.oled_detected = True
                    self.oled_last_error = ""
                    return True
            self.oled_detected = False
            self.oled_addr = 0
            self.oled_last_error = "not_detected"
            return False
        except Exception as exc:
            self.oled_detected = False
            self.oled_addr = 0
            self.oled_last_error = str(exc)
            return False

    def _scale(self, color):
        r, g, b = color
        k = self.brightness

        return (
            int(r * k),
            int(g * k),
            int(b * k)
        )

    def _clamp_brightness(self, value):
        try:
            value = float(value)
        except Exception:
            value = 0.35
        if value <= 0:
            return 0.10
        return max(0.10, min(0.50, value))

    def _scale_external(self, color, factor=1.0):
        factor = max(0.0, min(1.0, factor))
        k = self.external_brightness * factor
        return (
            int(max(0, min(255, color[0])) * k),
            int(max(0, min(255, color[1])) * k),
            int(max(0, min(255, color[2])) * k),
        )

    def set_color(self, color):
        if self.np is None:
            return

        scaled = self._scale(color)

        for i in range(board_pins.SK6812_COUNT):
            self.np[i] = scaled

        self.np.write()

    def off(self):
        self.state = "off"
        self.set_color((0, 0, 0))
        self._write_external(((0, 0, 0),) * int(getattr(board_pins, "WS2812B_COUNT", 0)), force=True)

    def set_booting(self):
        self.set_boot_window()

    def set_boot_window(self):
        self._set_state("boot_window")

    def set_normal(self):
        self._set_state("normal")

    def set_online(self):
        # compatibility alias
        self.set_normal()

    def set_scanning(self):
        # compatibility alias
        self.set_normal()

    def set_charging(self):
        self._set_state("charging")

    def set_charge_done(self):
        self._set_state("charge_done")

    def set_offline_recording(self, bucket="ok"):
        if bucket not in ("ok", "low", "critical", "urgent", "rolling", "error"):
            bucket = "ok"
        if bucket == "error":
            self.set_error()
            return
        self._set_state("offline_record_" + bucket)

    def set_offline_unavailable(self):
        self._set_state("offline_record_unavailable")

    def set_error(self):
        self._set_state("error")

    def set_wifi_setup(self):
        self._set_state("wifi_setup")

    def set_findme_no_gateway(self):
        self._set_state("findme_no_gateway")

    def set_findme_gateway_lost(self):
        self._set_state("findme_gateway_lost")

    def set_findme_rejected(self):
        self._set_state("findme_rejected")

    def set_updating(self):
        self._set_state("updating")

    def set_reboot_required(self):
        self._set_state("reboot_required")

    def set_maintenance(self):
        self._set_state("maintenance")

    def set_calibration(self):
        self._set_state("calibration")

    def update(self):
        now = time.ticks_ms()
        if self.np is not None:
            state_data = self.state_map.get(self.state, self.state_map["off"])
            intervals = state_data["intervals"]
            colors = state_data["colors"]

            if time.ticks_diff(now, self.last_update_ms) >= intervals[self.phase]:
                self.last_update_ms = now
                self.phase = (self.phase + 1) % len(intervals)
                self.brightness = state_data["brightness"]
                self.set_color(colors[self.phase])

        self._update_external(now)
        self._update_oled(now)

    def _set_state(self, state):
        if self.state == state:
            return
        self.state = state
        self.phase = 0
        self.last_update_ms = time.ticks_ms()
        state_data = self.state_map.get(state, self.state_map["off"])
        self.brightness = state_data["brightness"]
        self.set_color(state_data["colors"][0])

    def _active_external_preset(self):
        if self.external_mode != "on":
            return "off"
        return self.external_manual_preset if self.external_manual_preset in self.MANUAL_PRESETS else "stream_health"

    def _pulse(self, now, period_ms=1000, low=0.25, high=1.0):
        period_ms = max(1, int(period_ms))
        phase = (now % period_ms) / period_ms
        wave = phase * 2 if phase < 0.5 else (1.0 - phase) * 2
        return low + (high - low) * wave

    def _update_external(self, now):
        if self.external_mode != "on" or self.external_np is None:
            return
        self.external_active_preset = self._active_external_preset()
        colors = self._external_colors(now, self.external_active_preset)
        self._write_external(colors)

    def _external_colors(self, now, preset):
        ctx = self.indicator_context
        if preset == "recording_focus":
            rec = ctx.get("offline_recording", {})
            if not isinstance(rec, dict):
                rec = {}
            active = bool(rec.get("active"))
            rolling = bool(rec.get("rolling"))
            dropped = int(rec.get("dropped_frames") or 0)
            seconds = int(rec.get("estimated_seconds_until_rollover") or 0)
            led0 = self._scale_external((255, 0, 180) if active else (80, 80, 80), self._pulse(now, 1200, 0.35, 1.0))
            led1 = self._scale_external(self._storage_bucket_color(seconds), self._pulse(now, 1600, 0.45, 1.0))
            led2_color = (0, 180, 180) if rolling else ((255, 70, 0) if dropped else (0, 40, 0))
            led2 = self._scale_external(led2_color, self._pulse(now, 600 if dropped else 1800, 0.30, 1.0))
            return (led0, led1, led2)
        if preset == "calibration_focus":
            heap_free = int(ctx.get("heap_free") or 0)
            led0 = self._scale_external((200, 0, 140), self._pulse(now, 700, 0.25, 1.0))
            led1 = self._scale_external((0, 170, 70) if ctx.get("scan_ready") else (255, 160, 0), self._pulse(now, 1000, 0.35, 0.9))
            led2 = self._scale_external((255, 0, 0) if heap_free and heap_free < 20000 else (0, 80, 180), self._pulse(now, 1200, 0.30, 0.9))
            return (led0, led1, led2)
        if preset == "pressure_activity":
            max_p = float(ctx.get("pressure_max") or 0)
            motion = float(ctx.get("imu_motion") or 0)
            cop = float(ctx.get("cop_activity") or 0)
            led0 = self._scale_external(self._pressure_color(max_p), self._pulse(now, 900, 0.35, 1.0))
            led1 = self._scale_external((0, 180, 180), min(1.0, 0.25 + cop))
            led2 = self._scale_external((120, 0, 255), min(1.0, 0.25 + motion))
            return (led0, led1, led2)
        if preset == "stream_health_idle":
            if ctx.get("findme_rejected"):
                return (
                    self._scale_external((0, 80, 120), self._pulse(now, 2400, 0.20, 0.55)),
                    self._scale_external((180, 0, 255), self._pulse(now, 420, 0.35, 1.0)),
                    self._scale_external((255, 0, 0), self._pulse(now, 420, 0.35, 1.0)),
                )
            if ctx.get("findme_gateway_lost"):
                return (
                    self._scale_external((0, 80, 120), self._pulse(now, 2400, 0.20, 0.55)),
                    self._scale_external((255, 160, 0), self._pulse(now, 650, 0.35, 1.0)),
                    self._scale_external((220, 0, 0), self._pulse(now, 650, 0.35, 1.0)),
                )
            if ctx.get("findme_no_gateway"):
                return (
                    self._scale_external((0, 80, 120), self._pulse(now, 2400, 0.20, 0.55)),
                    self._scale_external((255, 90, 0), self._pulse(now, 500, 0.35, 1.0)),
                    self._scale_external((220, 0, 0), self._pulse(now, 500, 0.35, 1.0)),
                )
            return (
                self._scale_external((0, 80, 120), self._pulse(now, 2400, 0.20, 0.55)),
                self._scale_external((0, 120, 80) if ctx.get("control_connected") else (180, 0, 0), self._pulse(now, 1800, 0.30, 0.80)),
                self._scale_external((0, 0, 0), 0.0),
            )
        target_fps = float(ctx.get("target_fps") or 60)
        current_fps = float(ctx.get("current_fps") or 0)
        ratio = current_fps / target_fps if target_fps > 0 and current_fps > 0 else (1.0 if ctx.get("scan_active") else 0.0)
        dropped = int(ctx.get("dropped_frames") or 0) + int(ctx.get("failed_sends") or 0)
        led0_color = (0, 180, 80) if ratio >= 0.85 else ((255, 180, 0) if ctx.get("scan_active") else (0, 80, 120))
        led1_color = (0, 180, 120) if ctx.get("control_connected") else (220, 0, 0)
        led2_color = (0, 0, 0) if dropped <= 0 else ((255, 160, 0) if dropped < 10 else (255, 0, 0))
        return (
            self._scale_external(led0_color, self._pulse(now, 900, 0.35, 1.0)),
            self._scale_external(led1_color, self._pulse(now, 1200, 0.35, 0.9)),
            self._scale_external(led2_color, self._pulse(now, 500 if dropped else 1600, 0.25, 1.0)),
        )

    def _storage_bucket_color(self, seconds):
        if seconds <= 0:
            return (120, 0, 255)
        if seconds < 10:
            return (255, 40, 0)
        if seconds < 30:
            return (255, 110, 0)
        if seconds < 60:
            return (255, 200, 0)
        return (120, 0, 255)

    def _pressure_color(self, value):
        if value <= 300:
            return (0, 160, 120)
        if value >= 1000:
            return (255, 0, 0)
        ratio = (value - 300) / 700
        return (int(255 * ratio), int(200 * (1 - ratio)), 40)

    def _write_external(self, colors, force=False):
        if self.external_np is None:
            return
        count = int(getattr(board_pins, "WS2812B_COUNT", 0))
        if count <= 0:
            return
        colors = tuple(colors[:count])
        if not force and colors == self.external_last_colors:
            return
        for idx in range(count):
            self.external_np[idx] = colors[idx] if idx < len(colors) else (0, 0, 0)
        self.external_np.write()
        self.external_last_colors = colors

    def _init_oled(self):
        if not self.oled_detected or self.oled_mode == "off":
            return False
        if self.oled_initialized and self.oled_fb is not None:
            self._oled_cmd(0x81)
            self._oled_cmd(self.oled_contrast)
            return True
        try:
            import framebuf
            width = int(getattr(board_pins, "SSD1306_WIDTH", 128))
            height = int(getattr(board_pins, "SSD1306_HEIGHT", 32))
            self.oled_buffer = bytearray((width * height) // 8)
            self.oled_fb = framebuf.FrameBuffer(self.oled_buffer, width, height, framebuf.MONO_VLSB)
            self.oled_framebuf = framebuf
            for cmd in (
                0xAE,
                0x20, 0x00,
                0x40,
                0xA1,
                0xC8,
                0xDA, 0x02,
                0x81, self.oled_contrast,
                0xA4,
                0xA6,
                0xD5, 0x80,
                0x8D, 0x14,
                0xAF,
            ):
                self._oled_cmd(cmd)
            self.oled_initialized = True
            self.oled_last_error = ""
            self._render_oled(force=True)
            return True
        except Exception as exc:
            self.oled_initialized = False
            self.oled_last_error = str(exc)
            self.oled_fb = None
            self.oled_buffer = None
            return False

    def _clear_oled(self):
        if self.oled_fb is None:
            return
        try:
            self.oled_fb.fill(0)
            self._oled_show()
        except Exception:
            pass

    def _update_oled(self, now):
        if self.oled_mode == "off" or not self.oled_detected:
            return
        if not self.oled_initialized and not self._init_oled():
            return
        interval = int(1000 / max(1, int(self.oled_update_hz or 1)))
        if time.ticks_diff(now, self.oled_last_update_ms) < interval:
            return
        self.oled_last_update_ms = now
        self._render_oled()

    def _render_oled(self, force=False):
        if self.oled_fb is None:
            return False
        ctx = self.indicator_context
        try:
            self.oled_fb.fill(0)
            if self.oled_page == "sensor_snapshot":
                self._oled_text("NHOS {}".format(ctx.get("mode", "-")), 0, 0)
                self._oled_text("Pmax {}".format(self._fmt(ctx.get("pressure_max", 0), 0)), 0, 10)
                self._oled_text("IMU {}".format(self._fmt(ctx.get("imu_motion", 0), 2)), 0, 20)
            elif self.oled_page == "recording_status":
                rec = ctx.get("offline_recording", {})
                if not isinstance(rec, dict):
                    rec = {}
                self._oled_text("REC {}".format("ON" if rec.get("active") else "OFF"), 0, 0)
                self._oled_text("Seg {}".format(int(rec.get("segment_count") or 0)), 0, 10)
                self._oled_text("Left {}s".format(int(rec.get("estimated_seconds_until_rollover") or 0)), 0, 20)
            else:
                self._oled_text("NHOS {}".format(ctx.get("mode", "-")), 0, 0)
                self._oled_text("FPS {}".format(self._fmt(ctx.get("current_fps", 0), 1)), 0, 10)
                link = "UDP" if ctx.get("control_connected") else "NO UDP"
                self._oled_text("{} {}x{}".format(link, int(ctx.get("rows") or 0), int(ctx.get("cols") or 0)), 0, 20)
            self._oled_show()
            return True
        except Exception as exc:
            self.oled_last_error = str(exc)
            return False

    def _oled_text(self, text, x, y):
        self.oled_fb.text(str(text)[:21], x, y, 1)

    def _fmt(self, value, digits=1):
        try:
            return ("{:.%df}" % int(digits)).format(float(value))
        except Exception:
            return "0"

    def _oled_cmd(self, cmd):
        if self.oled_i2c is None or not self.oled_addr:
            return
        self.oled_i2c.writeto(self.oled_addr, bytes((0x80, int(cmd) & 0xFF)))

    def _oled_show(self):
        if self.oled_i2c is None or self.oled_buffer is None:
            return
        width = int(getattr(board_pins, "SSD1306_WIDTH", 128))
        height = int(getattr(board_pins, "SSD1306_HEIGHT", 32))
        pages = height // 8
        for cmd in (0x21, 0, width - 1, 0x22, 0, pages - 1):
            self._oled_cmd(cmd)
        chunk = 32
        buf = self.oled_buffer
        for offset in range(0, len(buf), chunk):
            self.oled_i2c.writeto(self.oled_addr, bytes((0x40,)) + buf[offset:offset + chunk])
