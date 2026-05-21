import importlib.util
import sys
import tempfile
import types
import unittest
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[1]
OS_ROOT = REPO_ROOT / "device" / "os"


def load_os_module(filename, module_name, config=None):
    saved = {}
    for name in ("config", "fs_core", "storage", module_name):
        saved[name] = sys.modules.get(name)
        sys.modules.pop(name, None)
    if config is not None:
        sys.modules["config"] = config
    sys.path.insert(0, str(OS_ROOT))
    try:
        spec = importlib.util.spec_from_file_location(module_name, OS_ROOT / filename)
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
        return module, saved
    except Exception:
        restore_modules(saved)
        raise
    finally:
        sys.path.pop(0)


def restore_modules(saved):
    for name, module in saved.items():
        if module is None:
            sys.modules.pop(name, None)
        else:
            sys.modules[name] = module


class OfflineRecorderTests(unittest.TestCase):
    def test_fs_core_rejects_path_escape_and_does_not_import_storage(self):
        module, saved = load_os_module("fs_core.py", "fs_core_test_module")
        try:
            with self.assertRaises(ValueError):
                module.safe_join("data/offline", "../device_state")
            with self.assertRaises(ValueError):
                module.safe_join("data/offline", "/device_state")
            with self.assertRaises(ValueError):
                module.safe_join("data/offline", "")
            self.assertEqual(module.safe_join("data/offline", "a/b.nhr"), "data/offline/a/b.nhr")
            self.assertNotIn("storage", sys.modules)
        finally:
            restore_modules(saved)

    def test_offline_recorder_rotates_and_deletes_oldest_segment(self):
        config = types.SimpleNamespace(
            OFFLINE_RECORD_SEGMENT_BYTES=32,
            OFFLINE_RECORD_RESERVE_BYTES=0,
            OFFLINE_RECORD_RESERVE_PERCENT=0,
            OFFLINE_RECORD_MIN_USABLE_BYTES=1,
            OFFLINE_RECORD_FLUSH_INTERVAL_MS=0,
            OFFLINE_RECORD_ESTIMATE_INTERVAL_MS=1,
            OFFLINE_RECORD_WRITE_BUDGET_MS=1000,
            OFFLINE_RECORD_WRITE_BACKOFF_MS=0,
        )
        module, saved = load_os_module("offline_recorder.py", "offline_recorder_test_module", config)
        try:
            with tempfile.TemporaryDirectory() as tmpdir:
                module.fs_core.statvfs_usage = lambda _path="/": {
                    "total_bytes": 128,
                    "used_bytes": 0,
                    "free_bytes": 72,
                    "percent_used": 0,
                }
                recorder = module.OfflineRecorder(str(Path(tmpdir) / "offline"))

                ok, message = recorder.begin(0)
                self.assertTrue(ok, message)

                packet = b"\x5a\xa5" + (b"x" * 14)
                for index in range(6):
                    self.assertTrue(recorder.write_packet(packet, index * 10))

                recorder.stop("test_done")
                files = sorted(path.name for path in (Path(tmpdir) / "offline").glob("*.nhr"))
                self.assertLessEqual(len(files), 3)
                self.assertTrue(recorder.rolling)
                self.assertGreater(recorder.bytes_used, 0)
                self.assertEqual(recorder.stop_reason, "test_done")
                self.assertNotIn("storage", sys.modules)
        finally:
            restore_modules(saved)

    def test_partial_tail_keeps_valid_records_decodable_by_length_header(self):
        config = types.SimpleNamespace(
            OFFLINE_RECORD_SEGMENT_BYTES=128,
            OFFLINE_RECORD_RESERVE_BYTES=0,
            OFFLINE_RECORD_RESERVE_PERCENT=0,
            OFFLINE_RECORD_MIN_USABLE_BYTES=1,
            OFFLINE_RECORD_FLUSH_INTERVAL_MS=0,
            OFFLINE_RECORD_ESTIMATE_INTERVAL_MS=1,
            OFFLINE_RECORD_WRITE_BUDGET_MS=1000,
            OFFLINE_RECORD_WRITE_BACKOFF_MS=0,
        )
        module, saved = load_os_module("offline_recorder.py", "offline_recorder_tail_test_module", config)
        try:
            with tempfile.TemporaryDirectory() as tmpdir:
                module.fs_core.statvfs_usage = lambda _path="/": {
                    "total_bytes": 256,
                    "used_bytes": 0,
                    "free_bytes": 256,
                    "percent_used": 0,
                }
                recorder = module.OfflineRecorder(str(Path(tmpdir) / "offline"))
                self.assertTrue(recorder.begin(0)[0])
                packet = b"\x5a\xa5payload"
                self.assertTrue(recorder.write_packet(packet, 10))
                recorder.stop("done")

                path = next((Path(tmpdir) / "offline").glob("*.nhr"))
                with path.open("ab") as handle:
                    handle.write(b"\x08")
                data = path.read_bytes()
                length = data[0] | (data[1] << 8)
                inverse = data[2] | (data[3] << 8)
                self.assertEqual(length ^ 0xFFFF, inverse)
                self.assertEqual(data[4:4 + length], packet)
        finally:
            restore_modules(saved)


if __name__ == "__main__":
    unittest.main()
