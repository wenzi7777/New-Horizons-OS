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
                "intervals": (350, 350),
                "colors": ((0, 0, 40), (0, 0, 0)),
                "brightness": 0.04,
            },
            "provisioning": {
                "intervals": (180, 820),
                "colors": ((0, 80, 80), (0, 0, 0)),
                "brightness": 0.08,
            },
            "normal": {
                "intervals": (120, 120, 120, 5000),
                "colors": ((0, 120, 0), (0, 0, 0), (0, 60, 0), (0, 0, 0)),
                "brightness": 0.06,
            },
            "updating": {
                "intervals": (90, 90, 90, 90, 90, 1500),
                "colors": ((0, 0, 120), (0, 0, 0), (0, 0, 80), (0, 0, 0), (0, 0, 40), (0, 0, 0)),
                "brightness": 0.08,
            },
            "calibration": {
                "intervals": (120, 120, 120, 120, 2000),
                "colors": ((120, 0, 120), (0, 0, 0), (60, 0, 60), (0, 0, 0), (0, 0, 0)),
                "brightness": 0.08,
            },
            "error": {
                "intervals": (70, 70, 70, 70, 70, 1500),
                "colors": ((120, 0, 0), (0, 0, 0), (80, 0, 0), (0, 0, 0), (40, 0, 0), (0, 0, 0)),
                "brightness": 0.1,
            },
            "charging": {
                "intervals": (300, 3000),
                "colors": ((120, 0, 0), (0, 0, 0)),
                "brightness": 0.05,
            },
            "charge_done": {
                "intervals": (150, 150, 2500),
                "colors": ((0, 120, 0), (0, 0, 0), (0, 0, 0)),
                "brightness": 0.05,
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

    def set_error(self):
        self._set_state("error")

    def set_provisioning(self):
        self._set_state("provisioning")

    def set_updating(self):
        self._set_state("updating")

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
