import importlib.util
import sys
import types
import unittest
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[1]
BOOT_PATH = REPO_ROOT / "device" / "root" / "boot.py"


def load_boot_module(injected_modules):
    saved_modules = {}
    for name, module in injected_modules.items():
        saved_modules[name] = sys.modules.get(name)
        sys.modules[name] = module

    try:
        spec = importlib.util.spec_from_file_location("boot_test_module", BOOT_PATH)
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
        return module, saved_modules
    except Exception:
        for name, saved in saved_modules.items():
            if saved is None:
                sys.modules.pop(name, None)
            else:
                sys.modules[name] = saved
        raise


class RootBootTests(unittest.TestCase):
    def test_boot_defaults_missing_layout_dirs(self):
        ensured = []
        injected = {
            "immutable_config": types.SimpleNamespace(
                DEVICE_STATE_DIR="device_state",
                RECOVERY_DIR="recovery",
            ),
            "uos": types.SimpleNamespace(mkdir=lambda path: ensured.append(path)),
        }

        _module, saved_modules = load_boot_module(injected)
        try:
            self.assertIn("nhos", ensured)
            self.assertIn("ota_stage", ensured)
            self.assertIn("data/files", ensured)
            self.assertIn("data/logs", ensured)
            self.assertIn("data/tmp", ensured)
        finally:
            for name, saved in saved_modules.items():
                if saved is None:
                    sys.modules.pop(name, None)
                else:
                    sys.modules[name] = saved

    def test_boot_does_not_import_recovery_storage(self):
        injected = {
            "immutable_config": types.SimpleNamespace(
                DEVICE_STATE_DIR="device_state",
                RECOVERY_DIR="recovery",
            ),
            "uos": types.SimpleNamespace(mkdir=lambda _path: None),
        }
        saved_storage = sys.modules.pop("storage", None)
        try:
            _module, saved_modules = load_boot_module(injected)
            self.assertNotIn("storage", sys.modules)
        finally:
            for name, saved in locals().get("saved_modules", {}).items():
                if saved is None:
                    sys.modules.pop(name, None)
                else:
                    sys.modules[name] = saved
            if saved_storage is not None:
                sys.modules["storage"] = saved_storage


if __name__ == "__main__":
    unittest.main()
