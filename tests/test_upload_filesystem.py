import importlib.util
import tempfile
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
    def test_recovery_upload_resets_network_state(self):
        module = load_upload_filesystem()

        paths = module.stale_device_paths("recovery", target_only=False)

        self.assertIn("device_state/network_config.json", paths)

    def test_recovery_target_only_preserves_network_state(self):
        module = load_upload_filesystem()

        paths = module.stale_device_paths("recovery", target_only=True)

        self.assertNotIn("device_state/network_config.json", paths)

    def test_os_upload_does_not_reset_network_state(self):
        module = load_upload_filesystem()

        paths = module.stale_device_paths("os", target_only=False)

        self.assertEqual(paths, [])

    def test_all_upload_resets_network_state(self):
        module = load_upload_filesystem()

        paths = module.stale_device_paths("all", target_only=False)

        self.assertIn("device_state/network_config.json", paths)

    def test_new_layout_upload_layers(self):
        module = load_upload_filesystem()

        self.assertEqual(
            [(item.name, item.remote_root) for item in module.target_layers("recovery", target_only=False)],
            [("root", ""), ("recovery", "recovery")],
        )
        self.assertEqual(
            [(item.name, item.remote_root) for item in module.target_layers("os", target_only=False)],
            [("os", "os")],
        )

    def test_upload_tree_creates_remote_root_before_nested_dirs(self):
        module = load_upload_filesystem()
        calls = []
        module.remote_mkdir = lambda port, path: calls.append(("mkdir", path))
        module.remote_copy = lambda port, local_path, remote_path: calls.append(("copy", remote_path))

        with tempfile.TemporaryDirectory() as tmpdir:
            source = Path(tmpdir)
            (source / "umqtt").mkdir()
            (source / "umqtt" / "simple.py").write_text("x = 1\n", encoding="utf-8")

            module.upload_tree("/dev/test", source, "recovery")

        self.assertEqual(calls[0], ("mkdir", "recovery"))
        self.assertIn(("mkdir", "recovery/umqtt"), calls)
        self.assertIn(("copy", "recovery/umqtt/simple.py"), calls)


if __name__ == "__main__":
    unittest.main()
