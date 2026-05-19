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

        self.settle_us = config.MATRIX_SETTLE_US
        self.col_active_level = config.COL_ACTIVE_LEVEL
        self.col_inactive_level = config.COL_INACTIVE_LEVEL

        self.current_col_index = None

    def begin(self):
        if HAS_NATIVE:
            print("Using native matrix scanner")
            matrix_scan_native.init(self.rows, self.cols, self.active_count)
            return

        print("Using optimized MicroPython matrix scanner")
        self._begin_python()

    def scan_once(self):
        if HAS_NATIVE:
            return matrix_scan_native.scan_once()

        return self._scan_once_python()

    def _begin_python(self):
        self._validate_active_pins()

        self.row_adcs = []
        for pin_no in self.active_row_pins:
            adc = machine.ADC(machine.Pin(pin_no))

            if config.ADC_ATTEN_11DB:
                try:
                    adc.atten(machine.ADC.ATTN_11DB)
                except Exception as e:
                    print("ADC atten warning on GPIO", pin_no, ":", e)

            try:
                adc.width(machine.ADC.WIDTH_12BIT)
            except Exception:
                pass

            self.row_adcs.append(adc)

        self.col_pins = []
        for pin_no in self.active_col_pins:
            pin = machine.Pin(pin_no, machine.Pin.OUT)
            pin.value(self.col_inactive_level)
            self.col_pins.append(pin)

        self._all_cols_off()

        print("Matrix scanner initialized")
        print("Available row GPIOs:", self.available_row_pins)
        print("Available col GPIOs:", self.available_col_pins)
        print("Active row GPIOs:", self.active_row_pins)
        print("Active col GPIOs:", self.active_col_pins)
        print("Active output points:", self.active_count)
        print("Settle us:", self.settle_us)

    def _validate_active_pins(self):
        for pin in self.active_row_pins:
            if pin not in self.available_row_pins:
                raise ValueError("ACTIVE_ROWS contains unavailable GPIO: {}".format(pin))

        for pin in self.active_col_pins:
            if pin not in self.available_col_pins:
                raise ValueError("ACTIVE_COLS contains unavailable GPIO: {}".format(pin))

    def _all_cols_off(self):
        inactive = self.col_inactive_level
        for pin in self.col_pins:
            pin.value(inactive)
        self.current_col_index = None

    def _select_col_by_index(self, active_col_index):
        # Fast switching:
        # turn off previous selected column only, then turn on new one.
        prev = self.current_col_index

        if prev is not None and prev != active_col_index:
            self.col_pins[prev].value(self.col_inactive_level)

        self.col_pins[active_col_index].value(self.col_active_level)
        self.current_col_index = active_col_index

    def _scan_once_python(self):
        frame = self.frame
        row_adcs = self.row_adcs
        col_pins = self.col_pins

        active_col_count = len(col_pins)
        active_row_count = len(row_adcs)

        settle_us = self.settle_us
        active_level = self.col_active_level
        inactive_level = self.col_inactive_level

        current_col = self.current_col_index

        # Full output fast path
        if self.active_count == active_row_count * active_col_count:
            for col_index in range(active_col_count):
                if current_col is not None and current_col != col_index:
                    col_pins[current_col].value(inactive_level)

                col_pins[col_index].value(active_level)
                current_col = col_index

                if settle_us > 0:
                    time.sleep_us(settle_us)

                # row-major output:
                # R0C0, R0C1, ..., R1C0, R1C1 ...
                for row_index in range(active_row_count):
                    out_index = row_index * active_col_count + col_index
                    frame[out_index] = row_adcs[row_index].read()

            if current_col is not None:
                col_pins[current_col].value(inactive_level)

            self.current_col_index = None
            return frame

        # Partial output path
        out_limit = self.active_count

        for col_index in range(active_col_count):
            if current_col is not None and current_col != col_index:
                col_pins[current_col].value(inactive_level)

            col_pins[col_index].value(active_level)
            current_col = col_index

            if settle_us > 0:
                time.sleep_us(settle_us)

            for row_index in range(active_row_count):
                out_index = row_index * active_col_count + col_index

                if out_index < out_limit:
                    frame[out_index] = row_adcs[row_index].read()

        if current_col is not None:
            col_pins[current_col].value(inactive_level)

        self.current_col_index = None
        return frame