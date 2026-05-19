# matrix_scan.py
from array import array
import time
import machine

import config

try:
    import matrix_scan_native
    HAS_NATIVE = True
except ImportError:
    HAS_NATIVE = False


class MatrixScanner:
    def __init__(self, rows, cols, active_count=None):
        self.rows = rows
        self.cols = cols

        # Physical GPIO numbers
        self.available_row_pins = list(config.AVAILABLE_ROWS)
        self.available_col_pins = list(config.AVAILABLE_COLS)

        self.active_row_pins = list(config.ACTIVE_ROWS)
        self.active_col_pins = list(config.ACTIVE_COLS)

        self.total_active_count = len(self.active_row_pins) * len(self.active_col_pins)

        if active_count is None:
            active_count = config.ACTIVE_SENSOR_COUNT

        if active_count is None:
            self.active_count = self.total_active_count
        else:
            self.active_count = min(active_count, self.total_active_count)

        self.frame = array("H", [0] * self.active_count)

        self.row_adcs = []
        self.col_pins = []
        self.row_pin_map = []
        self.col_pin_map = []

        self.settle_us = config.MATRIX_SETTLE_US
        self.col_active_level = config.COL_ACTIVE_LEVEL
        self.col_inactive_level = config.COL_INACTIVE_LEVEL

    def begin(self):
        if HAS_NATIVE:
            print("matrix_scan_backend=native rows={} cols={} points={}".format(
                self.rows,
                self.cols,
                self.active_count,
            ))
            matrix_scan_native.init(self.rows, self.cols, self.active_count)
            return

        print("matrix_scan_backend=micropython rows={} cols={} points={}".format(
            self.rows,
            self.cols,
            self.active_count,
        ))
        self._begin_python()

    def scan_once(self):
        if HAS_NATIVE:
            return matrix_scan_native.scan_once()

        return self._scan_once_python()

    def sample_cell(self, analog_pin, select_pin, duration_ms=1000):
        if HAS_NATIVE:
            raise NotImplementedError("native calibration sampling not implemented")

        row_index = self.row_pin_map.index(int(analog_pin))
        col_index = self.col_pin_map.index(int(select_pin))
        adc = self.row_adcs[row_index]
        pin = self.col_pins[col_index]

        started = time.ticks_ms()
        total = 0.0
        count = 0
        while time.ticks_diff(time.ticks_ms(), started) < int(duration_ms):
            self._activate_col(pin)
            if self.settle_us > 0:
                time.sleep_us(self.settle_us)
            total += self._read_mv(adc)
            count += 1
            self._release_col(pin)
            time.sleep_ms(1)

        self._all_cols_off()
        if count == 0:
            return None
        return total / count

    def _begin_python(self):
        self._validate_active_pins()

        self.row_adcs = []
        self.row_pin_map = []

        for pin_no in self.active_row_pins:
            try:
                pin = machine.Pin(pin_no)
                adc = machine.ADC(pin)

                if config.ADC_ATTEN_11DB:
                    try:
                        adc.atten(machine.ADC.ATTN_11DB)
                    except Exception as e:
                        print("ADC atten warning on GPIO", pin_no, ":", e)

                try:
                    adc.width(machine.ADC.WIDTH_12BIT)
                except Exception as e:
                    print("ADC width warning on GPIO", pin_no, ":", e)

                # Test read once
                if getattr(config, "PRINT_MATRIX_INIT_DETAILS", False):
                    try:
                        v = adc.read()
                        print("ROW ADC GPIO", pin_no, "test read:", v)
                    except Exception as e:
                        print("ROW ADC GPIO", pin_no, "test read failed:", e)

                self.row_adcs.append(adc)
                self.row_pin_map.append(pin_no)

            except Exception as e:
                print("FAILED: ROW ADC GPIO", pin_no, ":", repr(e))
                raise

        self.col_pins = []
        self.col_pin_map = []

        for pin_no in self.active_col_pins:
            try:
                pin = self._make_col_pin(pin_no)
                self.col_pins.append(pin)
                self.col_pin_map.append(pin_no)

            except Exception as e:
                print("FAILED: COL GPIO", pin_no, ":", repr(e))
                raise

        self._all_cols_off()

        print("matrix_scan_initialized rows={} cols={} points={} settle_us={}".format(
            len(self.active_row_pins),
            len(self.active_col_pins),
            self.active_count,
            self.settle_us,
        ))

    def _validate_active_pins(self):
        for pin in self.active_row_pins:
            if pin not in self.available_row_pins:
                raise ValueError("ACTIVE_ROWS contains unavailable GPIO: {}".format(pin))

        for pin in self.active_col_pins:
            if pin not in self.available_col_pins:
                raise ValueError("ACTIVE_COLS contains unavailable GPIO: {}".format(pin))

    def _all_cols_off(self):
        for pin in self.col_pins:
            self._release_col(pin)

    def _select_col_by_index(self, active_col_index):
        self._all_cols_off()
        self._activate_col(self.col_pins[active_col_index])

    def _scan_once_python(self):
        out_limit = self.active_count
        active_col_count = len(self.active_col_pins)

        for col_index in range(active_col_count):
            self._select_col_by_index(col_index)

            if self.settle_us > 0:
                time.sleep_us(self.settle_us)

            for row_index, adc in enumerate(self.row_adcs):
                out_index = row_index * active_col_count + col_index

                if out_index >= out_limit:
                    continue

                self.frame[out_index] = self._read_mv(adc)

        self._all_cols_off()
        return self.frame

    def _make_col_pin(self, pin_no):
        open_drain = getattr(machine.Pin, "OPEN_DRAIN", None)
        if open_drain is not None:
            pin = machine.Pin(pin_no, open_drain)
            try:
                pin.value(1)
            except Exception:
                pass
            return pin
        pin = machine.Pin(pin_no, machine.Pin.OUT)
        pin.value(self.col_inactive_level)
        return pin

    def _release_col(self, pin):
        try:
            pin.value(1)
        except Exception:
            pin.value(self.col_inactive_level)

    def _activate_col(self, pin):
        try:
            pin.value(0)
        except Exception:
            pin.value(self.col_active_level)

    def _read_mv(self, adc):
        try:
            return float(adc.read_uv()) / 1000.0
        except Exception:
            pass

        try:
            raw_u16 = float(adc.read_u16())
            return (raw_u16 / 65535.0) * 3300.0
        except Exception:
            pass

        try:
            raw = float(adc.read())
            return (raw / 4095.0) * 3300.0
        except Exception:
            return 0.0
