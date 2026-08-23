import importlib.util
import json
import os
import subprocess
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
                changelog_url="https://example.com/notes/v1.2.3.md",
            )

            self.assertEqual(manifest["product"], "New Horizons OS Arduino")
            self.assertEqual(manifest["protocol"], "NHO/Arduino/1")
            self.assertEqual(manifest["model"], "VD-CTL/R v1.0.F 2026.4")
            self.assertEqual(manifest["latest"], "v1.2.3")
            self.assertEqual(manifest["changelog_url"], "https://example.com/notes/v1.2.3.md")
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
                changelog_url="https://example.com/notes/v9.9.9.md",
            )

            raw = output.read_text(encoding="utf-8")
            self.assertTrue(raw.endswith("\n"))
            decoded = json.loads(raw)
            self.assertEqual(decoded["latest"], "v9.9.9")
            self.assertEqual(decoded["changelog_url"], "https://example.com/notes/v9.9.9.md")
            self.assertEqual(decoded["firmware"]["size"], 8)

    def test_v15f_release_track_emits_its_own_artifact_and_manifest(self):
        script = REPO_ROOT / "firmware" / "scripts" / "build_arduino_release_v15f.sh"
        with tempfile.TemporaryDirectory() as tmpdir:
            tmp = Path(tmpdir)
            fake_bin = tmp / "bin"
            fake_bin.mkdir()
            fake_cli = fake_bin / "arduino-cli"
            fake_cli.write_text(
                "#!/bin/sh\n"
                "out=\"\"\n"
                "while [ $# -gt 0 ]; do\n"
                "  if [ \"$1\" = \"--output-dir\" ]; then out=$2; shift 2; continue; fi\n"
                "  shift\n"
                "done\n"
                "mkdir -p \"$out\"\n"
                "printf firmware > \"$out/newhorizons_os.ino.bin\"\n",
                encoding="utf-8",
            )
            fake_cli.chmod(0o755)
            release_dir = tmp / "releases"
            build_dir = tmp / "build"
            result = subprocess.run(
                [str(script)],
                check=True,
                text=True,
                capture_output=True,
                env={
                    **os.environ,
                    "PATH": f"{fake_bin}{os.pathsep}{os.environ['PATH']}",
                    "VERSION": "v9.8.7",
                    "RELEASE_DIR": str(release_dir),
                    "OUT_DIR": str(build_dir),
                },
            )
            artifact = release_dir / "newhorizons-os-v15f-v9.8.7.bin"
            latest = release_dir.parent / "arduino-v15f-latest.json"
            versioned = release_dir.parent / "arduino-v15f-v9.8.7.json"
            self.assertEqual(Path(result.stdout.strip()), artifact)
            self.assertEqual(artifact.read_bytes(), b"firmware")
            self.assertEqual(json.loads(latest.read_text(encoding="utf-8"))["latest"], "v9.8.7")
            self.assertEqual(json.loads(versioned.read_text(encoding="utf-8"))["model"], "VD-CTL/R v1.5.F 2026.7")


if __name__ == "__main__":
    unittest.main()
