import importlib.util
import sys
import tempfile
import unittest
from pathlib import Path


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
            REPO_ROOT / "device" / "immutable" / "device_logging.py",
            "immutable_device_logging_streaming_test",
        ),
        (
            REPO_ROOT / "device" / "channels" / "full" / "files" / "device_logging.py",
            "full_device_logging_streaming_test",
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


if __name__ == "__main__":
    unittest.main()
