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
    add("ACTION_BUTTON_PIN", ACTION_BUTTON_PIN)

    conflicts = {}
    for pin, names in used.items():
        if len(names) > 1:
            conflicts[pin] = names

    return conflicts