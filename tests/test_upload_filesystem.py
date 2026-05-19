import contextlib
import io
import importlib.util
import subprocess
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

    def test_remote_copy_falls_back_to_raw_exec_when_os_stat_is_missing(self):
        module = load_upload_filesystem()
        calls = []

        def fake_run(cmd, check=False, capture_output=False, text=False):
            calls.append(cmd)
            if cmd[:4] == ["mpremote", "connect", "/dev/test", "fs"]:
                return subprocess.CompletedProcess(
                    cmd,
                    1,
                    "",
                    "AttributeError: 'module' object has no attribute 'stat'\n",
                )
            return subprocess.CompletedProcess(cmd, 0, "", "")

        module.subprocess.run = fake_run
        module.RAW_COPY_CHUNK_SIZE = 4
        with tempfile.TemporaryDirectory() as tmpdir:
            local_path = Path(tmpdir) / "udp_control.py"
            local_path.write_bytes(b"abcdef")

            with contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO()):
                module.remote_copy("/dev/test", local_path, "recovery/udp_control.py")

        exec_commands = [cmd for cmd in calls if cmd[:3] == ["mpremote", "connect", "/dev/test"] and cmd[3] == "exec"]
        self.assertEqual(exec_commands[0][4], "f=open('recovery/udp_control.py','wb');f.close()")
        self.assertIn("b'abcd'", exec_commands[1][4])
        self.assertIn("b'ef'", exec_commands[2][4])


if __name__ == "__main__":
    unittest.main()
