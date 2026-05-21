# sk6812.py
import time
import machine
import neopixel

import board_pins


class SK6812Status:
    def __init__(self):
        self.np = None
        self.state = "off"

        self.brightness = 0.08

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

    def _scale(self, color):
        r, g, b = color
        k = self.brightness

        return (
            int(r * k),
            int(g * k),
            int(b * k)
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

    def set_updating(self):
        self._set_state("updating")

    def set_reboot_required(self):
        self._set_state("reboot_required")

    def set_maintenance(self):
        self._set_state("maintenance")

    def set_calibration(self):
        self._set_state("calibration")

    def update(self):
        if self.np is None:
            return

        now = time.ticks_ms()
        state_data = self.state_map.get(self.state, self.state_map["off"])
        intervals = state_data["intervals"]
        colors = state_data["colors"]

        if time.ticks_diff(now, self.last_update_ms) < intervals[self.phase]:
            return

        self.last_update_ms = now
        self.phase = (self.phase + 1) % len(intervals)
        self.brightness = state_data["brightness"]
        self.set_color(colors[self.phase])

    def _set_state(self, state):
        if self.state == state:
            return
        self.state = state
        self.phase = 0
        self.last_update_ms = time.ticks_ms()
        state_data = self.state_map.get(state, self.state_map["off"])
        self.brightness = state_data["brightness"]
        self.set_color(state_data["colors"][0])
