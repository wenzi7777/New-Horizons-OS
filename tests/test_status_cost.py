"""`status` and `storage_status` keep their filesystem work off the hot path.

Measured on a v1.5.F: a SPIFFS open just to read the log size cost `status`
~10 ms of main-loop time on every call, and storage_status walked the whole
flat filesystem once per category (~0.5 s). Both run on the main loop.
"""
import unittest
from pathlib import Path

ARDUINO_ROOT = Path(__file__).resolve().parents[1] / "firmware" / "newhorizons_os"


def body(text: str, signature: str) -> str:
    start = text.index(signature)
    return text[start:text.index("\n}\n", start)]


class StatusCostTests(unittest.TestCase):
    def setUp(self):
        self.storage = (ARDUINO_ROOT / "Storage.cpp").read_text(encoding="utf-8")

    def test_log_status_reads_the_cached_size(self):
        log_status = body(self.storage, "String Storage::logStatusJson() const {")
        self.assertNotIn("SPIFFS.open", log_status)
        self.assertIn("currentLogBytes()", log_status)

    def test_log_size_cache_is_kept_or_invalidated_on_every_write(self):
        self.assertIn("noteLogWritten(current.size());", body(self.storage, "void Storage::logTagged("))
        self.assertIn("noteLogWritten(0);", body(self.storage, "void Storage::clearLog()"))
        rotate = body(self.storage, "void Storage::rotateLogIfNeeded(")
        self.assertIn("noteLogWritten(currentSize);", rotate)
        self.assertIn("noteLogWritten(0);", rotate)
        self.assertIn("forgetLogBytes();", body(self.storage, "bool Storage::writeFile("))
        self.assertIn("forgetLogBytes();", body(self.storage, "bool Storage::deleteFile("))

    def test_storage_status_walks_the_filesystem_once(self):
        status = body(self.storage, "String Storage::storageStatusJson()")
        self.assertEqual(status.count("SPIFFS.open("), 1)
        self.assertIn("accumulateCategories(root, bytes);", status)
        self.assertNotIn("directorySize(", self.storage)



class BatteryStatusCommandTests(unittest.TestCase):
    def test_battery_status_returns_only_the_battery_cards_fields(self):
        server = (ARDUINO_ROOT / "ControlServer.cpp").read_text(encoding="utf-8")
        start = server.index('if (cmd == "battery_status")')
        handler = server[start:server.index('if (cmd == "resync_battery_gauge")', start)]
        for key in ('"battery"', '"battery_gauge"', '"power"'):
            self.assertIn(key, handler)
        self.assertEqual(handler.count("jsonRawField("), 3)


if __name__ == "__main__":
    unittest.main()
