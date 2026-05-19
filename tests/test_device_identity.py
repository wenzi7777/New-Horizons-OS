import importlib.util
import sys
import types
import unittest
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[1]


def load_device_identity(relative_path="device/recovery/device_identity.py", injected_modules=None):
    injected_modules = injected_modules or {}
    saved_modules = {}
    for name, module in injected_modules.items():
        saved_modules[name] = sys.modules.get(name)
        sys.modules[name] = module
    path = REPO_ROOT / relative_path
    spec = importlib.util.spec_from_file_location("device_identity_test", path)
    module = importlib.util.module_from_spec(spec)
    try:
        spec.loader.exec_module(module)
        return module, saved_modules
    except Exception:
        for name, saved in saved_modules.items():
            if saved is None:
                sys.modules.pop(name, None)
            else:
                sys.modules[name] = saved
        raise


def restore_modules(saved_modules):
    for name, saved in saved_modules.items():
        if saved is None:
            sys.modules.pop(name, None)
        else:
            sys.modules[name] = saved


class DeviceIdentityTests(unittest.TestCase):
    def test_device_id_is_derived_from_mac_bytes(self):
        module, saved_modules = load_device_identity()
        self.addCleanup(restore_modules, saved_modules)

        device_id = module.derive_device_id(b"\xaa\xbb\xcc\xdd\xee\xff")

        self.assertIsInstance(device_id, int)
        self.assertNotEqual(device_id, 0)
        self.assertNotEqual(device_id, 0x00000001)
        self.assertEqual(device_id, module.derive_device_id(b"\xaa\xbb\xcc\xdd\xee\xff"))
        self.assertEqual(device_id, 0xCCDDEEFF)

    def test_device_uid_is_uppercase_hex(self):
        module, saved_modules = load_device_identity()
        self.addCleanup(restore_modules, saved_modules)

        self.assertEqual(module.mac_hex(b"\x01\x0a\xff"), "010AFF")

    def test_runtime_identity_uses_machine_unique_id_without_initializing_wifi(self):
        for relative_path in ("device/recovery/device_identity.py", "device/os/device_identity.py"):
            with self.subTest(relative_path=relative_path):
                wlan_calls = []

                def wlan(iface):
                    wlan_calls.append(iface)
                    return types.SimpleNamespace(config=lambda name: b"\x00\x00\x00\x00\x00\x00")

                module, saved_modules = load_device_identity(
                    relative_path,
                    {
                        "machine": types.SimpleNamespace(unique_id=lambda: b"\xaa\xbb\xcc\xdd\xee\xff"),
                        "network": types.SimpleNamespace(STA_IF=0, AP_IF=1, WLAN=wlan),
                    },
                )
                self.addCleanup(restore_modules, saved_modules)

                self.assertEqual(module.get_device_uid(), "AABBCCDDEEFF")
                self.assertEqual(module.get_device_id(), 0xCCDDEEFF)
                self.assertEqual(wlan_calls, [])


if __name__ == "__main__":
    unittest.main()
