import importlib.util
import sys
import tempfile
import unittest
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[1]
OS_DIR = REPO_ROOT / "device" / "os"


def load_storage_module():
    old_path = list(sys.path)
    sys.path.insert(0, str(OS_DIR))
    try:
        spec = importlib.util.spec_from_file_location("os_storage_tlv_test", OS_DIR / "storage.py")
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
        return module
    finally:
        sys.path[:] = old_path


class TLVStorageTests(unittest.TestCase):
    def test_storage_round_trips_nested_tlv_without_json_import(self):
        storage = load_storage_module()
        source = (OS_DIR / "storage.py").read_text(encoding="utf-8")

        self.assertNotIn("import json", source)
        self.assertTrue(hasattr(storage, "load_tlv"))
        self.assertTrue(hasattr(storage, "save_tlv"))

        payload = {
            "mode": "normal",
            "scan_timing": {"target_fps": 90, "send_every_n_frames": 2},
            "active_rows": [1, 2, 3],
            "enabled": True,
        }
        with tempfile.TemporaryDirectory() as tmpdir:
            path = Path(tmpdir) / "runtime_config.tlv"
            storage.save_tlv(str(path), payload)
            raw = path.read_bytes()

            self.assertTrue(raw.startswith(storage.TLV_MAGIC))
            self.assertEqual(storage.load_tlv(str(path), {}), payload)


if __name__ == "__main__":
    unittest.main()
