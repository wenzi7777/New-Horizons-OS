import builtins
import importlib.util
import sys
import types
import unittest
from io import BytesIO
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[1]
BMI270_DRIVER_PATH = (
    REPO_ROOT
    / "device"
    / "channels"
    / "full"
    / "files"
    / "micropython_bmi270"
    / "bmi270.py"
)


def load_driver_module():
    saved_modules = {}
    injected = {
        "micropython": types.SimpleNamespace(const=lambda value: value),
        "micropython_bmi270.i2c_helpers": types.SimpleNamespace(
            CBits=lambda *args, **kwargs: None,
            RegisterStruct=lambda *args, **kwargs: None,
        ),
    }
    for name, module in injected.items():
        saved_modules[name] = sys.modules.get(name)
        sys.modules[name] = module

    try:
        spec = importlib.util.spec_from_file_location("bmi270_streaming_test", BMI270_DRIVER_PATH)
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


class FakeSensor:
    def __init__(self):
        self.internal_status = 0
        self._address = 0x68
        self._power_configuration = None
        self._init_control = None
        self._init_address_0 = None
        self._init_address_1 = None
        self.writes = []
        self._i2c = types.SimpleNamespace(
            writeto_mem=lambda address, register, payload: self.writes.append(
                (address, register, payload)
            )
        )


class BMI270ConfigStreamingTests(unittest.TestCase):
    def test_load_config_file_streams_binary_asset_without_importing_large_python_list(self):
        module, saved_modules = load_driver_module()
        original_open = builtins.open
        opened_paths = []
        try:
            module.time.sleep = lambda _seconds: None

            def fake_open(path, mode="r", *args, **kwargs):
                opened_paths.append((path, mode))
                if path == module.CONFIG_FILE_PATH and mode == "rb":
                    return BytesIO(bytes(range(32)) * 256)
                return original_open(path, mode, *args, **kwargs)

            builtins.open = fake_open
            sensor = FakeSensor()

            module.BMI270.load_config_file(sensor)
        finally:
            builtins.open = original_open
            for name, saved in saved_modules.items():
                if saved is None:
                    sys.modules.pop(name, None)
                else:
                    sys.modules[name] = saved

        self.assertEqual(opened_paths, [(module.CONFIG_FILE_PATH, "rb")])
        self.assertEqual(len(sensor.writes), 256)
        self.assertTrue(all(len(payload) == 32 for _addr, _reg, payload in sensor.writes))


if __name__ == "__main__":
    unittest.main()
