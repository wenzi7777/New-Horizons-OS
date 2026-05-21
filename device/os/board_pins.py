# board_pins.py

# Matrix
# ROW lines are ADC inputs.
# COL lines are digital select / excitation outputs.

ROW_ADC_PINS = [1, 2, 3, 4, 5, 6, 7, 8, 9, 10]

COL_PINS = [
    13, 14, 15, 16, 17, 18, 19, 20, 21, 26,
    47, 33, 34, 48, 35, 36, 37, 38, 39, 40, 41
]

# I2C
I2C_SCL = 42
I2C_SDA = 45

# LED
SK6812_PIN = 11
SK6812_COUNT = 1
WS2812B_PIN = 12
WS2812B_COUNT = 3

# Optional SSD1306 128x32 display on the shared I2C bus.
SSD1306_WIDTH = 128
SSD1306_HEIGHT = 32
SSD1306_ADDR_PRIMARY = 0x3C
SSD1306_ADDR_FALLBACK = 0x3D

# Button
ACTION_BUTTON_PIN = 46


def validate_pins():
    used = {}

    def add(name, pins):
        if isinstance(pins, int):
            pins = [pins]
        for p in pins:
            used.setdefault(p, []).append(name)

    add("ROW_ADC_PINS", ROW_ADC_PINS)
    add("COL_PINS", COL_PINS)
    add("I2C_SCL", I2C_SCL)
    add("I2C_SDA", I2C_SDA)
    add("SK6812_PIN", SK6812_PIN)
    add("WS2812B_PIN", WS2812B_PIN)
    add("ACTION_BUTTON_PIN", ACTION_BUTTON_PIN)

    conflicts = {}
    for pin, names in used.items():
        if len(names) > 1:
            conflicts[pin] = names

    return conflicts
