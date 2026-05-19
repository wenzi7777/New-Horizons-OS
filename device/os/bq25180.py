# bq25180.py
import config
import i2c_bus


class BQ25180:
    # Registers
    REG_STAT0 = 0x00
    REG_STAT1 = 0x01
    REG_FLAG0 = 0x02
    REG_VBAT_CTRL = 0x03
    REG_ICHG_CTRL = 0x04
    REG_CHARGE_CTRL0 = 0x05
    REG_CHARGE_CTRL1 = 0x06
    REG_IC_CTRL = 0x07
    REG_TMR_ILIM = 0x08
    REG_SHIP_RST = 0x09
    REG_SYS_REG = 0x0A
    REG_TS_CONTROL = 0x0B
    REG_MASK_ID = 0x0C

    # STAT0
    STAT0_VIN_PGOOD = 0x01
    STAT0_CHG_STAT_MASK = 0x60
    STAT0_CHG_STAT_SHIFT = 5

    CHG_NOT_CHARGING = 0
    CHG_CONSTANT_CURRENT = 1
    CHG_CONSTANT_VOLTAGE = 2
    CHG_DONE = 3

    # ICHG_CTRL
    ICHG_CHG_DIS = 0x80
    ICHG_MASK = 0x7F

    STATUS_UNKNOWN = 0
    STATUS_NOT_CHARGING = 1
    STATUS_CHARGING_CC = 2
    STATUS_CHARGING_CV = 3
    STATUS_DONE = 4
    STATUS_FAULT = 5

    def __init__(self):
        self.i2c = None
        self.addr = config.BQ25180_ADDR
        self.available = False

        self.last_stat0 = 0
        self.last_stat1 = 0
        self.last_flag0 = 0
        self.last_status_code = self.STATUS_UNKNOWN

    def begin(self):
        print("BQ25180 begin")

        self.i2c = i2c_bus.get_i2c()

        try:
            devices = self.i2c.scan()
            print("I2C devices:", [hex(d) for d in devices])

            if self.addr not in devices:
                print("BQ25180 not found at", hex(self.addr))
                self.available = False
                return False

            self.available = True
            print("BQ25180 found at", hex(self.addr))

            # 允許充電
            self.set_charge_enabled(True)

            # 設定充電電流與電壓
            self.set_charge_current_ma(config.BQ25180_CHARGE_CURRENT_MA)
            self.set_charge_voltage_mv(config.BQ25180_CHARGE_VOLTAGE_MV)

            self.dump_registers()
            return True

        except Exception as e:
            print("BQ25180 init failed:", e)
            self.available = False
            return False

    def _read_u8(self, reg):
        data = self.i2c.readfrom_mem(self.addr, reg, 1)
        return data[0]

    def _write_u8(self, reg, value):
        self.i2c.writeto_mem(self.addr, reg, bytes([value & 0xFF]))

    def _update_bits(self, reg, mask, value):
        old = self._read_u8(reg)
        new = (old & (~mask & 0xFF)) | (value & mask)

        if new != old:
            self._write_u8(reg, new)

        return new

    def set_charge_enabled(self, enable):
        if not self.available:
            return False

        if enable:
            # CHG_DIS = 0
            self._update_bits(self.REG_ICHG_CTRL, self.ICHG_CHG_DIS, 0x00)
        else:
            # CHG_DIS = 1
            self._update_bits(
                self.REG_ICHG_CTRL,
                self.ICHG_CHG_DIS,
                self.ICHG_CHG_DIS
            )

        return True

    def _current_ma_to_code(self, current_ma):
        # BQ2518x 常見 ICHG encoding:
        # 5mA~35mA: code = ICHG - 5
        # >35mA: code = ((ICHG - 40) / 10) + 31
        if current_ma < 5:
            current_ma = 5
        if current_ma > 1000:
            current_ma = 1000

        if current_ma <= 35:
            return int(current_ma - 5)

        return int((current_ma - 40) // 10 + 31)

    def set_charge_current_ma(self, current_ma):
        if not self.available:
            return False

        code = self._current_ma_to_code(current_ma)
        self._update_bits(self.REG_ICHG_CTRL, self.ICHG_MASK, code)

        print("BQ25180 charge current set:", current_ma, "mA, code:", code)
        return True

    def _voltage_mv_to_code(self, voltage_mv):
        # VBATREG = 3500mV + code * 10mV
        if voltage_mv < 3500:
            voltage_mv = 3500
        if voltage_mv > 4650:
            voltage_mv = 4650

        return int((voltage_mv - 3500) // 10)

    def set_charge_voltage_mv(self, voltage_mv):
        if not self.available:
            return False

        code = self._voltage_mv_to_code(voltage_mv)
        self._update_bits(self.REG_VBAT_CTRL, 0x7F, code)

        print("BQ25180 charge voltage set:", voltage_mv, "mV, code:", code)
        return True

    def read_status(self):
        """
        Return tuple for packet.py:
        (status_code, fault, vbat_mv)

        注意：BQ25180 不是 fuel gauge。
        這裡 vbat_mv 暫時回傳 0。
        """
        if not self.available:
            self.last_status_code = self.STATUS_UNKNOWN
            return None

        try:
            stat0 = self._read_u8(self.REG_STAT0)
            stat1 = self._read_u8(self.REG_STAT1)
            flag0 = self._read_u8(self.REG_FLAG0)

            self.last_stat0 = stat0
            self.last_stat1 = stat1
            self.last_flag0 = flag0

            vin_good = (stat0 & self.STAT0_VIN_PGOOD) != 0
            chg_stat = (
                stat0 & self.STAT0_CHG_STAT_MASK
            ) >> self.STAT0_CHG_STAT_SHIFT

            if not vin_good:
                status = self.STATUS_NOT_CHARGING
            elif chg_stat == self.CHG_CONSTANT_CURRENT:
                status = self.STATUS_CHARGING_CC
            elif chg_stat == self.CHG_CONSTANT_VOLTAGE:
                status = self.STATUS_CHARGING_CV
            elif chg_stat == self.CHG_DONE:
                status = self.STATUS_DONE
            else:
                status = self.STATUS_NOT_CHARGING

            self.last_status_code = status

            # 先把 flag0 原樣作為 fault 傳出去，方便 PC 端 debug。
            fault = flag0

            return (status, fault, 0)

        except Exception as e:
            print("BQ25180 read_status failed:", e)
            self.last_status_code = self.STATUS_UNKNOWN
            return None

    def is_vin_good(self):
        return (self.last_stat0 & self.STAT0_VIN_PGOOD) != 0

    def is_charging(self):
        return self.last_status_code in (
            self.STATUS_CHARGING_CC,
            self.STATUS_CHARGING_CV
        )

    def is_charge_done(self):
        return self.last_status_code == self.STATUS_DONE

    def status_name(self):
        s = self.last_status_code

        if s == self.STATUS_CHARGING_CC:
            return "charging_cc"
        if s == self.STATUS_CHARGING_CV:
            return "charging_cv"
        if s == self.STATUS_DONE:
            return "done"
        if s == self.STATUS_NOT_CHARGING:
            return "not_charging"
        if s == self.STATUS_FAULT:
            return "fault"

        return "unknown"

    def dump_registers(self):
        if not self.available:
            return

        try:
            print("BQ25180 registers:")
            for reg in range(0x00, 0x0D):
                val = self._read_u8(reg)
                print("  0x%02X = 0x%02X" % (reg, val))
        except Exception as e:
            print("BQ25180 dump failed:", e)