import importlib.util
import unittest
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[1]
SCRIPT_PATH = REPO_ROOT / "firmware" / "scripts" / "upload_filesystem.py"


def load_upload_filesystem():
    spec = importlib.util.spec_from_file_location("upload_filesystem_test", SCRIPT_PATH)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


class UploadFilesystemTests(unittest.TestCase):
    def test_minimal_full_flash_resets_network_state(self):
        module = load_upload_filesystem()

        paths = module.stale_device_paths("minimal", channel_only=False)

        self.assertIn("device_state/network_config.json", paths)

    def test_minimal_channel_only_preserves_network_state(self):
        module = load_upload_filesystem()

        paths = module.stale_device_paths("minimal", channel_only=True)

        self.assertNotIn("device_state/network_config.json", paths)

    def test_full_flash_does_not_reset_network_state(self):
        module = load_upload_filesystem()

        paths = module.stale_device_paths("full", channel_only=False)

        self.assertEqual(paths, [])


if __name__ == "__main__":
    unittest.main()
