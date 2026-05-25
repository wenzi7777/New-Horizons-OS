import importlib.util
import json
import tempfile
import unittest
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[1]
SCRIPT_PATH = REPO_ROOT / "firmware" / "scripts" / "generate_arduino_manifest.py"


def load_manifest_module():
    spec = importlib.util.spec_from_file_location("arduino_manifest_test_module", SCRIPT_PATH)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


class ArduinoManifestGeneratorTests(unittest.TestCase):
    def test_build_manifest_records_model_version_size_and_sha256(self):
        module = load_manifest_module()
        with tempfile.TemporaryDirectory() as tmpdir:
            tmp = Path(tmpdir)
            firmware = tmp / "newhorizons-os-v1.2.3.bin"
            firmware.write_bytes(b"arduino-firmware")

            manifest = module.build_manifest(
                firmware_path=firmware,
                model="VD-CTL/R v1.0.F 2026.4",
                version="v1.2.3",
                base_url="https://example.com/releases/v1.2.3",
            )

            self.assertEqual(manifest["product"], "New Horizons OS Arduino")
            self.assertEqual(manifest["protocol"], "NHO/Arduino/1")
            self.assertEqual(manifest["model"], "VD-CTL/R v1.0.F 2026.4")
            self.assertEqual(manifest["latest"], "v1.2.3")
            self.assertEqual(manifest["firmware"]["size"], len(b"arduino-firmware"))
            self.assertEqual(
                manifest["firmware"]["url"],
                "https://example.com/releases/v1.2.3/newhorizons-os-v1.2.3.bin",
            )
            self.assertRegex(manifest["firmware"]["sha256"], r"^[0-9a-f]{64}$")

    def test_write_manifest_outputs_stable_json(self):
        module = load_manifest_module()
        with tempfile.TemporaryDirectory() as tmpdir:
            tmp = Path(tmpdir)
            firmware = tmp / "firmware.bin"
            output = tmp / "arduino-latest.json"
            firmware.write_bytes(b"firmware")

            module.write_manifest(
                output_path=output,
                firmware_path=firmware,
                model="VD-CTL/R v1.0.F 2026.4",
                version="v9.9.9",
                base_url="https://example.com",
            )

            raw = output.read_text(encoding="utf-8")
            self.assertTrue(raw.endswith("\n"))
            decoded = json.loads(raw)
            self.assertEqual(decoded["latest"], "v9.9.9")
            self.assertEqual(decoded["firmware"]["size"], 8)


if __name__ == "__main__":
    unittest.main()
