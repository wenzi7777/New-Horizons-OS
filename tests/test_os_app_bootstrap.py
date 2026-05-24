import importlib.util
import sys
import types
import unittest
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[1]
APP_PATH = REPO_ROOT / "device" / "os" / "app.py"


def load_app_module(injected_modules):
    saved_modules = {}
    for name, module in injected_modules.items():
        saved_modules[name] = sys.modules.get(name)
        sys.modules[name] = module

    try:
        spec = importlib.util.spec_from_file_location("os_app_bootstrap_test_module", APP_PATH)
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
        module.time.ticks_ms = lambda: 0
        return module, saved_modules
    except Exception:
        for name, saved in saved_modules.items():
            if saved is None:
                sys.modules.pop(name, None)
            else:
                sys.modules[name] = saved
        raise


class OSAppBootstrapTests(unittest.TestCase):
    def test_app_run_drops_recovery_nhcp_before_loading_core(self):
        observations = {}
        recovery_nhcp = types.SimpleNamespace(marker="recovery-nhcp")

        class CoreApp:
            def __init__(self, wifi_setup_requested=False):
                self.wifi_setup_requested = wifi_setup_requested

            def run(self):
                observations["nhcp_module"] = sys.modules.get("nhcp")
                observations["wifi_setup_requested"] = self.wifi_setup_requested

        injected = {
            "app_core": types.SimpleNamespace(App=CoreApp),
            "nhcp": recovery_nhcp,
        }
        module, saved_modules = load_app_module(injected)
        try:
            module.App(wifi_setup_requested=True).run()
        finally:
            for name, saved in saved_modules.items():
                if saved is None:
                    sys.modules.pop(name, None)
                else:
                    sys.modules[name] = saved

        self.assertIsNot(observations["nhcp_module"], recovery_nhcp)
        self.assertTrue(observations["wifi_setup_requested"])


if __name__ == "__main__":
    unittest.main()
