import importlib.util
import unittest
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[1]


def load_device_identity():
    path = REPO_ROOT / "device" / "recovery" / "device_identity.py"
    spec = importlib.util.spec_from_file_location("device_identity_test", path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


class DeviceIdentityTests(unittest.TestCase):
    def test_device_id_is_derived_from_mac_bytes(self):
        module = load_device_identity()

        device_id = module.derive_device_id(b"\xaa\xbb\xcc\xdd\xee\xff")

        self.assertIsInstance(device_id, int)
        self.assertNotEqual(device_id, 0)
        self.assertNotEqual(device_id, 0x00000001)
        self.assertEqual(device_id, module.derive_device_id(b"\xaa\xbb\xcc\xdd\xee\xff"))
        self.assertEqual(device_id, 0xCCDDEEFF)

    def test_device_uid_is_uppercase_hex(self):
        module = load_device_identity()

        self.assertEqual(module.mac_hex(b"\x01\x0a\xff"), "010AFF")


if __name__ == "__main__":
    unittest.main()
