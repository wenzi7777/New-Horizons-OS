import importlib.util
import json
import sys
import tempfile
import unittest
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[1]
SCRIPT_PATH = REPO_ROOT / "firmware" / "scripts" / "generate_manifest.py"


def load_generate_manifest_module():
    spec = importlib.util.spec_from_file_location("generate_manifest_test_module", SCRIPT_PATH)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


class GenerateManifestTests(unittest.TestCase):
    def test_os_manifest_uses_new_os_tree_and_skips_device_state(self):
        module = load_generate_manifest_module()
        with tempfile.TemporaryDirectory() as tmpdir:
            repo_root = Path(tmpdir)
            files_root = repo_root / "device" / "os"
            (files_root / ".device").mkdir(parents=True)
            (files_root / "device_state").mkdir(parents=True)
            (files_root / "app.py").write_text("print('ok')\n", encoding="utf-8")
            (files_root / ".device" / "runtime_config.json").write_text("{}", encoding="utf-8")
            (files_root / ".device" / "filter_config.json").write_text("{}", encoding="utf-8")
            (files_root / "device_state" / "runtime_config.json").write_text("{}", encoding="utf-8")
            (files_root / "device_state" / "filter_config.json").write_text("{}", encoding="utf-8")

            old_argv = sys.argv
            sys.argv = [
                "generate_manifest.py",
                "--repo-root",
                str(repo_root),
                "--target",
                "os",
                "--version",
                "v9.9.9",
            ]
            try:
                module.main()
            finally:
                sys.argv = old_argv

            manifest_path = repo_root / "device" / "os" / "manifest.json"
            manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
            self.assertEqual(manifest["type"], "os")
            self.assertEqual(manifest["target_root"], "/nhos")
            self.assertEqual(
                manifest["base_url"],
                "https://raw.githubusercontent.com/wenzi7777/New-Horizons-OS/v9.9.9/device/os",
            )
            self.assertEqual(
                [item["path"] for item in manifest["files"]],
                ["app.py"],
            )
            raw_manifest = manifest_path.read_text(encoding="utf-8")
            self.assertNotIn("\n  ", raw_manifest)
            self.assertLess(len(raw_manifest), len(json.dumps(manifest, indent=2)))

    def test_os_manifest_can_publish_mpy_artifacts_and_delete_source_py(self):
        module = load_generate_manifest_module()
        with tempfile.TemporaryDirectory() as tmpdir:
            repo_root = Path(tmpdir)
            source_root = repo_root / "device" / "os_mpy"
            delete_root = repo_root / "device" / "os"
            (source_root / "transport").mkdir(parents=True)
            (delete_root / "umqtt").mkdir(parents=True)
            (source_root / "app.mpy").write_bytes(b"mpy-app")
            (source_root / "transport" / "tcp_control.mpy").write_bytes(b"mpy-tcp")
            (source_root / "micropython_bmi270" / "config_file.bin").parent.mkdir(parents=True)
            (source_root / "micropython_bmi270" / "config_file.bin").write_bytes(b"bin")
            (delete_root / "app.py").write_text("print('old')\n", encoding="utf-8")
            (delete_root / "mqtt_transport.py").write_text("print('old')\n", encoding="utf-8")
            (delete_root / "umqtt" / "simple.py").write_text("print('old')\n", encoding="utf-8")
            (delete_root / "manifest.json").write_text("{}", encoding="utf-8")

            old_argv = sys.argv
            sys.argv = [
                "generate_manifest.py",
                "--repo-root",
                str(repo_root),
                "--target",
                "os",
                "--version",
                "v9.9.9",
                "--source-root",
                str(source_root),
                "--base-url-path",
                "device/os_mpy",
                "--delete-source-root",
                str(delete_root),
                "--delete-suffix",
                ".py",
                "--delete-path",
                "main.py",
                "--delete-path",
                "umqtt/__init__.py",
            ]
            try:
                module.main()
            finally:
                sys.argv = old_argv

            manifest_path = repo_root / "device" / "os" / "manifest.json"
            manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
            self.assertEqual(
                manifest["base_url"],
                "https://raw.githubusercontent.com/wenzi7777/New-Horizons-OS/v9.9.9/device/os_mpy",
            )
            self.assertEqual(
                [item["path"] for item in manifest["files"]],
                ["app.mpy", "micropython_bmi270/config_file.bin", "transport/tcp_control.mpy"],
            )
            self.assertEqual(manifest["delete"], ["app.py", "main.py", "mqtt_transport.py", "umqtt/__init__.py", "umqtt/simple.py"])

    def test_recovery_manifest_publishes_tcp_control_and_deletes_mqtt_files(self):
        manifest_path = REPO_ROOT / "device" / "recovery" / "manifest.json"
        manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
        paths = [item["path"] for item in manifest.get("files", [])]
        deletes = set(manifest.get("delete", []))

        self.assertIn("tcp_control.py", paths)
        self.assertNotIn("mqtt_transport.py", paths)
        self.assertTrue({"mqtt_transport.py", "umqtt/simple.py", "umqtt/robust.py", "umqtt/__init__.py"} <= deletes)

    def test_os_manifest_publishes_udp_tcp_modules_and_deletes_mqtt_files(self):
        manifest_path = REPO_ROOT / "device" / "os" / "manifest.json"
        manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
        paths = [item["path"] for item in manifest.get("files", [])]
        deletes = set(manifest.get("delete", []))

        self.assertIn("tcp_control.mpy", paths)
        self.assertIn("udp_stream.mpy", paths)
        self.assertIn("fs_core.mpy", paths)
        self.assertIn("offline_recorder.mpy", paths)
        self.assertNotIn("mqtt_transport.mpy", paths)
        self.assertTrue({
            "fs_core.py",
            "offline_recorder.py",
            "mqtt_transport.py",
            "umqtt/simple.py",
            "umqtt/robust.py",
            "umqtt/__init__.py",
        } <= deletes)

    def test_v0423_keeps_os_app_mpy_within_resource_budget(self):
        manifest_path = REPO_ROOT / "device" / "os" / "manifest.json"
        manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
        files = {item["path"]: item for item in manifest.get("files", [])}

        self.assertEqual(manifest["version"], "v0.4.23")
        self.assertEqual(manifest["firmware_version"], "v0.4.23")
        self.assertLess(files["app.mpy"]["size"], 2048)
        self.assertLess(files["app_core.mpy"]["size"], 50000)


if __name__ == "__main__":
    unittest.main()
