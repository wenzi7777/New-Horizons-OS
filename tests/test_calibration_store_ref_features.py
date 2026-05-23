import importlib.util
import sys
import types
import unittest
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[1]


def load_calibration_store():
    path = REPO_ROOT / "device" / "os" / "calibration_store.py"
    fake_storage = types.SimpleNamespace(
        load_tlv=lambda *_args, **_kwargs: {"points": {}},
        save_tlv=lambda *_args, **_kwargs: None,
    )
    saved_storage = sys.modules.get("storage")
    sys.modules["storage"] = fake_storage
    try:
        spec = importlib.util.spec_from_file_location("calibration_store_test", path)
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
        return module
    finally:
        if saved_storage is None:
            sys.modules.pop("storage", None)
        else:
            sys.modules["storage"] = saved_storage


class CalibrationStoreRefFeatureTests(unittest.TestCase):
    def test_lists_levels_in_sorted_order(self):
        module = load_calibration_store()
        store = module.CalibrationStore()
        store.set_point(1, 13, 1.5, 120.0)
        store.set_point(1, 13, 0.0, 30.0)

        self.assertEqual(store.list_levels(), ["0.000", "1.500"])

    def test_dumps_level_as_active_pin_matrix(self):
        module = load_calibration_store()
        store = module.CalibrationStore()
        store.set_point(1, 13, 0.0, 11.0)
        store.set_point(2, 20, 0.0, 22.0)

        dumped = store.dump_level(0.0, [1, 2], [13, 20])

        self.assertEqual(dumped["level"], "0.000")
        self.assertEqual(dumped["matrix"], [[11.0, None], [None, 22.0]])


if __name__ == "__main__":
    unittest.main()
