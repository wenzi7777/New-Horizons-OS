import importlib.util
import sys
import unittest
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[1]


def load_module(module_path: Path, module_name: str):
    sys.modules.pop("storage", None)
    sys.modules.pop("manifest_update", None)
    sys.path.insert(0, str(module_path.parent))
    try:
        spec = importlib.util.spec_from_file_location(module_name, module_path)
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
        return module
    finally:
        sys.path.pop(0)


class FakeConfigStore:
    def __init__(self):
        self.runtime = {
            "update": {
                "enabled": True,
                "manifest_url": "https://example.com/manifest.json",
            }
        }
        self.update_state = {
            "last_manifest_sha256": "",
            "last_check_ms": 0,
            "last_result": "",
            "reboot_required": False,
        }

    def load_runtime(self):
        return dict(self.runtime)

    def load_update_state(self):
        return dict(self.update_state)

    def save_update_state(self, state):
        self.update_state = dict(state)


class FakePlanner:
    def __init__(self):
        self.applied = []

    def plan(self, manifest):
        return {
            "downloads": [
                {
                    "path": "alpha.py",
                    "url": "https://example.com/alpha.py",
                    "sha256": "hash-alpha",
                },
                {
                    "path": "beta.py",
                    "url": "https://example.com/beta.py",
                    "sha256": "hash-beta",
                },
            ],
            "reboot_required": True,
        }

    def _local_path(self, relative_path):
        return relative_path


class UpdateManagerProgressTests(unittest.TestCase):
    def _assert_progress_flow(self, module_path: Path, module_name: str):
        module = load_module(module_path, module_name)
        store = FakeConfigStore()
        planner = FakePlanner()
        manager = module.UpdateManager(store, logger=None, root_dir=".")
        manager.planner = planner
        manager._fetch_json = lambda url: {"files": []}
        manager._download_to_path = lambda url, local_path, expected_sha256: planner.applied.append((local_path, url))

        start = manager.start_apply()
        self.assertEqual(start["message"], "update_started")
        self.assertEqual(store.update_state["phase"], "checking_manifest")

        first_tick = manager.service()
        self.assertEqual(first_tick["message"], "update_progress")
        self.assertEqual(store.update_state["phase"], "downloading")
        self.assertEqual(store.update_state["total_files"], 2)
        self.assertEqual(store.update_state["applied_files"], 0)

        second_tick = manager.service()
        self.assertEqual(second_tick["message"], "update_progress")
        self.assertEqual(store.update_state["applied_files"], 1)
        self.assertEqual(store.update_state["current_file"], "beta.py")

        final_tick = manager.service()
        self.assertEqual(final_tick["message"], "update_applied")
        self.assertEqual(store.update_state["phase"], "done")
        self.assertEqual(store.update_state["applied_files"], 2)
        self.assertTrue(store.update_state["reboot_required"])
        self.assertEqual(store.update_state["last_result"], "applied")
        self.assertEqual([item[0] for item in planner.applied], ["alpha.py", "beta.py"])

    def test_immutable_update_manager_reports_incremental_progress(self):
        self._assert_progress_flow(
            REPO_ROOT / "device" / "immutable" / "update_manager.py",
            "immutable_update_manager_test",
        )

    def test_full_update_manager_reports_incremental_progress(self):
        self._assert_progress_flow(
            REPO_ROOT / "device" / "channels" / "full" / "files" / "update_manager.py",
            "full_update_manager_test",
        )


if __name__ == "__main__":
    unittest.main()
