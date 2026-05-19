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
            self.assertEqual(manifest["target_root"], "/os")
            self.assertEqual(
                manifest["base_url"],
                "https://raw.githubusercontent.com/wenzi7777/New-Horizons-OS/v9.9.9/device/os",
            )
            self.assertEqual(
                [item["path"] for item in manifest["files"]],
                ["app.py"],
            )


if __name__ == "__main__":
    unittest.main()
