import time

import storage


DEBUG = 10
INFO = 20
WARN = 30
ERROR = 40


class DeviceLogger:
    def __init__(self, path=".device/logs/device.log", max_bytes=32768, level=INFO):
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
        existing = storage.read_text(self.path, "")
        combined = existing + line
        if len(combined.encode()) > self.max_bytes:
            combined = combined[-(self.max_bytes // 2):]
        storage.write_text(self.path, combined)

    def read_tail(self, max_lines=50):
        text = storage.read_text(self.path, "")
        if not text:
            return []
        lines = [line for line in text.splitlines() if line]
        return lines[-max_lines:]

    def _timestamp(self):
        try:
            return str(time.time())
        except Exception:
            return "0"
