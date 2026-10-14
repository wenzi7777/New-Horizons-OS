"""spec/wire.json is the one written copy of the wire constants.

firmware/scripts/gen_wire.py generates the firmware's and Hub's headers and
the Desktop's and Gateway's wire.py from it; this fails when a committed copy
in this repository (or a sibling checkout) is not what it would generate.
"""
import subprocess
import sys
import unittest
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]


class WireSpecTests(unittest.TestCase):
    def test_generated_files_are_current(self):
        result = subprocess.run(
            [sys.executable, str(REPO / "firmware" / "scripts" / "gen_wire.py"), "--check"],
            capture_output=True, text=True,
        )
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


if __name__ == "__main__":
    unittest.main()
