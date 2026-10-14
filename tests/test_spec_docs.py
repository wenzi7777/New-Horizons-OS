"""spec/commands.md and spec/config.md are generated from the firmware's tables."""
import subprocess
import sys
import unittest
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]


class SpecDocsTests(unittest.TestCase):
    def test_generated_docs_are_current(self):
        result = subprocess.run(
            [sys.executable, str(REPO / "firmware" / "scripts" / "gen_spec_docs.py"), "--check"],
            capture_output=True, text=True,
        )
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_protocol_overview_names_the_generated_files(self):
        overview = (REPO / "spec" / "PROTOCOL.md").read_text(encoding="utf-8")
        for name in ("wire.json", "commands.md", "config.md", "fixtures/versions.json", "fixtures/packets.json"):
            self.assertIn(name, overview)
            self.assertTrue((REPO / "spec" / name).exists(), name)


if __name__ == "__main__":
    unittest.main()
