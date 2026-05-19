import importlib.util
import io
import sys
import tempfile
import unittest
from pathlib import Path
from contextlib import redirect_stdout


REPO_ROOT = Path(__file__).resolve().parents[1]


def load_module(module_path: Path, module_name: str):
    sys.modules.pop("storage", None)
    sys.path.insert(0, str(module_path.parent))
    try:
        spec = importlib.util.spec_from_file_location(module_name, module_path)
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
        return module
    finally:
        sys.path.pop(0)


class DeviceLoggingStreamingTests(unittest.TestCase):
    MODULES = (
        (
            REPO_ROOT / "device" / "recovery" / "device_logging.py",
            "recovery_device_logging_streaming_test",
        ),
        (
            REPO_ROOT / "device" / "os" / "device_logging.py",
            "os_device_logging_streaming_test",
        ),
    )

    def test_logger_appends_and_reads_tail_without_bulk_reading_log(self):
        for module_path, module_name in self.MODULES:
            with self.subTest(module=module_name), tempfile.TemporaryDirectory() as tmpdir:
                module = load_module(module_path, module_name)
                log_path = Path(tmpdir) / "device.log"
                logger = module.DeviceLogger(str(log_path), max_bytes=128)
                module.storage.read_text = lambda *args, **kwargs: (_ for _ in ()).throw(
                    AssertionError("read_text should not be used for steady-state logging")
                )

                logger.info("first")
                logger.info("second")

                self.assertEqual(logger.read_tail(1)[0].split(" ", 2)[-1], "second")

    def test_logger_rotates_large_file_before_appending(self):
        for module_path, module_name in self.MODULES:
            with self.subTest(module=module_name), tempfile.TemporaryDirectory() as tmpdir:
                module = load_module(module_path, module_name)
                log_path = Path(tmpdir) / "device.log"
                log_path.write_text("x" * 120)
                logger = module.DeviceLogger(str(log_path), max_bytes=128)

                logger.info("latest")

                text = log_path.read_text()
                self.assertIn("latest", text)
                self.assertLessEqual(len(text.encode()), 128)

    def test_default_logging_capacity_uses_two_segments(self):
        for module_path, module_name in self.MODULES:
            with self.subTest(module=module_name), tempfile.TemporaryDirectory() as tmpdir:
                module = load_module(module_path, module_name)
                logger = module.DeviceLogger(str(Path(tmpdir) / "device.log"))

                self.assertTrue(logger.enabled)
                self.assertEqual(logger.capacity, "default")
                self.assertEqual(logger.max_bytes, 16384)
                self.assertEqual(logger.segment_bytes, 8192)

    def test_disabled_file_logging_still_prints_serial_status(self):
        for module_path, module_name in self.MODULES:
            with self.subTest(module=module_name), tempfile.TemporaryDirectory() as tmpdir:
                module = load_module(module_path, module_name)
                log_path = Path(tmpdir) / "device.log"
                logger = module.DeviceLogger(str(log_path), enabled=False)

                output = io.StringIO()
                with redirect_stdout(output):
                    logger.info("serial only")

                self.assertIn("serial only", output.getvalue())
                self.assertFalse(log_path.exists())

    def test_rotating_log_tail_reads_backup_then_active(self):
        for module_path, module_name in self.MODULES:
            with self.subTest(module=module_name), tempfile.TemporaryDirectory() as tmpdir:
                module = load_module(module_path, module_name)
                log_path = Path(tmpdir) / "device.log"
                logger = module.DeviceLogger(str(log_path), max_bytes=128)
                logger._timestamp = lambda: "1"

                for idx in range(12):
                    logger.info("line{}".format(idx))

                backup_path = Path(str(log_path) + ".1")
                self.assertTrue(backup_path.exists())
                self.assertLessEqual(len(log_path.read_bytes()), 64)
                self.assertLessEqual(len(backup_path.read_bytes()), 64)
                self.assertEqual(
                    [line.split(" ", 2)[-1] for line in logger.read_tail(4)],
                    ["line8", "line9", "line10", "line11"],
                )

    def test_log_lines_are_limited_before_writing(self):
        for module_path, module_name in self.MODULES:
            with self.subTest(module=module_name), tempfile.TemporaryDirectory() as tmpdir:
                module = load_module(module_path, module_name)
                log_path = Path(tmpdir) / "device.log"
                logger = module.DeviceLogger(str(log_path), max_bytes=1024)

                logger.info("x" * 400)

                line = log_path.read_text().splitlines()[0]
                self.assertLessEqual(len(line), 256)


if __name__ == "__main__":
    unittest.main()
