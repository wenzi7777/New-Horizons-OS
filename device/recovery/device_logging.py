try:
    import uos as os
except ImportError:  # pragma: no cover - CPython fallback
    import os
import time

import storage


DEBUG = 10
INFO = 20
WARN = 30
ERROR = 40


class DeviceLogger:
    def __init__(self, path="device_state/logs/device.log", max_bytes=32768, level=INFO):
        self.path = path
        self.max_bytes = int(max_bytes)
        self.level = int(level)

    def debug(self, message):
        self.log(DEBUG, "DEBUG", message)

    def info(self, message):
        self.log(INFO, "INFO", message)

    def warn(self, message):
        self.log(WARN, "WARN", message)

    def error(self, message):
        self.log(ERROR, "ERROR", message)

    def log(self, level_value, level_name, message):
        if int(level_value) < self.level:
            return
        line = "{} [{}] {}\n".format(self._timestamp(), level_name, message)
        print(line, end="")
        self._append_line(line)

    def read_tail(self, max_lines=50):
        limit = max(0, int(max_lines))
        if limit == 0:
            return []
        lines = []
        try:
            with open(self.path, "r") as f:
                for line in f:
                    line = line.rstrip("\n")
                    if not line:
                        continue
                    lines.append(line)
                    if len(lines) > limit:
                        lines.pop(0)
        except OSError:
            return []
        return lines

    def _append_line(self, line):
        encoded_len = len(line.encode())
        storage.ensure_dir(storage.dirname(self.path))
        current_size = self._file_size()
        mode = "a"
        if current_size + encoded_len > self.max_bytes:
            mode = "w"
        with open(self.path, mode) as f:
            f.write(line)

    def _file_size(self):
        try:
            return int(os.stat(self.path)[6])
        except OSError:
            return 0

    def _timestamp(self):
        try:
            return str(time.time())
        except Exception:
            return "0"
