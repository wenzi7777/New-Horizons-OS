# bmi270.py
import time
import struct
import gc

import config
import i2c_bus


class BMI270:
    TEMP_LSB_REG = 0x22
    TEMP_MSB_REG = 0x23

    def __init__(self):
        self.i2c = None
        self.addr = config.BMI270_ADDR
        self.available = False
        self.sensor = None
        self.driver = None

        self.ax = 0.0
        self.ay = 0.0
        self.az = 0.0
        self.gx = 0.0
        self.gy = 0.0
        self.gz = 0.0
        self.chip_temp = "NA"

    def begin(self):
        print("BMI270 begin")

        self.i2c = i2c_bus.get_i2c()

        try:
            devices = self.i2c.scan()
            print("I2C devices:", [hex(d) for d in devices])

            if self.addr not in devices:
                if config.BMI270_FALLBACK_ADDR in devices:
                    self.addr = config.BMI270_FALLBACK_ADDR
                else:
                    print("BMI270 not found")
                    self.available = False
                    return False

            from micropython_bmi270 import bmi270

            self.driver = bmi270
            self.sensor = bmi270.BMI270(self.i2c, address=self.addr)
            # The upstream constructor loads the BMI270 config file. It creates
            # short-lived buffers while streaming the 8 KiB blob, so collect
            # immediately before the rest of OS boot checks heap pressure.
            gc.collect()

            # Keep a guarded retry for driver variants that do not initialize in
            # the constructor, but avoid re-running the 8 KiB config path when
            # the sensor already reports initialized.
            try:
                if getattr(self.sensor, "internal_status", 0) != 0x01:
                    self.sensor.load_config_file()
                    gc.collect()
            except Exception as e:
                print("BMI270 load_config_file warning:", e)

            # 嘗試設定常用 range。不同版本 library 若沒有常數，也不影響基本讀取。
            try:
                self.sensor.acceleration_range = bmi270.ACCEL_RANGE_8G
            except Exception:
                pass

            try:
                self.sensor.gyro_range = bmi270.GYRO_RANGE_500
            except Exception:
                pass
            gc.collect()

            self.available = True
            print("BMI270 initialized at", hex(self.addr))
            return True

        except ImportError:
            print("BMI270 driver missing.")
            print("Install with:")
            print("mpremote mip install github:jposada202020/MicroPython_BMI270")
            self.available = False
            return False

        except Exception as e:
            print("BMI270 init failed:", e)
            self.available = False
            return False

    def read(self):
        """
        Return:
        (ax, ay, az, gx, gy, gz, chip_temp)

        ax/ay/az: m/s^2
        gx/gy/gz: deg/s
        chip_temp: degC or "NA"
        """
        if not self.available or self.sensor is None:
            return None

        try:
            ax, ay, az = self.sensor.acceleration
            gx, gy, gz = self.sensor.gyro

            self.ax = ax
            self.ay = ay
            self.az = az
            self.gx = gx
            self.gy = gy
            self.gz = gz
            self.chip_temp = self._read_chip_temp()

            return (
                self.ax,
                self.ay,
                self.az,
                self.gx,
                self.gy,
                self.gz,
                self.chip_temp
            )

        except Exception as e:
            print("BMI270 read failed:", e)
            return None

    def _read_chip_temp(self):
        """
        BMI270 溫度暫時用 raw register 讀取。
        若讀取失敗，回傳 "NA"。
        """
        try:
            data = self.i2c.readfrom_mem(self.addr, self.TEMP_LSB_REG, 2)
            raw = struct.unpack("<h", data)[0]

            # Bosch BMI2 系列常用換算：
            # temperature_degC = raw / 512 + 23
            temp = raw / 512.0 + 23.0
            return temp

        except Exception:
            return "NA"
