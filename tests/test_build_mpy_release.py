import os
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[1]
SCRIPT_PATH = REPO_ROOT / "firmware" / "scripts" / "build_mpy_release.py"


class BuildMpyReleaseTests(unittest.TestCase):
    def test_compiles_python_tree_to_mpy_and_copies_assets(self):
        with tempfile.TemporaryDirectory() as tmpdir:
            root = Path(tmpdir)
            source = root / "device" / "os"
            output = root / "device" / "os_mpy"
            fake_cross = root / "mpy-cross"
            (source / "umqtt").mkdir(parents=True)
            (source / "__pycache__").mkdir()
            (source / "app.py").write_text("print('app')\n", encoding="utf-8")
            (source / "umqtt" / "simple.py").write_text("print('simple')\n", encoding="utf-8")
            (source / "micropython_bmi270").mkdir()
            (source / "micropython_bmi270" / "config_file.bin").write_bytes(b"asset")
            (source / "__pycache__" / "app.pyc").write_bytes(b"skip")
            fake_cross.write_text(
                "#!/usr/bin/env python3\n"
                "import pathlib, sys\n"
                "out = pathlib.Path(sys.argv[sys.argv.index('-o') + 1])\n"
                "src = pathlib.Path(sys.argv[-1])\n"
                "out.parent.mkdir(parents=True, exist_ok=True)\n"
                "out.write_bytes(b'mpy:' + src.read_bytes())\n",
                encoding="utf-8",
            )
            fake_cross.chmod(0o755)

            result = subprocess.run(
                [
                    sys.executable,
                    str(SCRIPT_PATH),
                    "--source",
                    str(source),
                    "--output",
                    str(output),
                    "--mpy-cross",
                    str(fake_cross),
                ],
                text=True,
                capture_output=True,
            )

            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertTrue((output / "app.mpy").exists())
            self.assertTrue((output / "umqtt" / "simple.mpy").exists())
            self.assertFalse((output / "app.py").exists())
            self.assertFalse((output / "__pycache__").exists())
            self.assertEqual((output / "micropython_bmi270" / "config_file.bin").read_bytes(), b"asset")


if __name__ == "__main__":
    unittest.main()
