# i2c_bus.py
import machine
import board_pins

_I2C0 = None

# 同時給 BMI270 + BQ25180 使用，建議固定 400 kHz。
I2C_FREQ = 400000


def get_i2c():
    global _I2C0

    if _I2C0 is None:
        _I2C0 = machine.I2C(
            0,
            scl=machine.Pin(board_pins.I2C_SCL),
            sda=machine.Pin(board_pins.I2C_SDA),
            freq=I2C_FREQ
        )

    return _I2C0


def scan():
    return get_i2c().scan()