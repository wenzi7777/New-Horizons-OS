import re
import unittest
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[1]
NOTES = REPO_ROOT / "releases" / "notes"


class ReleaseNotesTests(unittest.TestCase):
    def test_current_release_has_notes_in_three_languages(self):
        # vX.Y.Z.md is the Traditional Chinese original every manifest links;
        # from v1.18.0 each release also ships .ja.md and .en.md beside it,
        # which Desktop and the GitHub Release pick up.
        config = (REPO_ROOT / "firmware" / "newhorizons_os" / "Config.h").read_text(encoding="utf-8")
        version = re.search(r'#define NHOS_FIRMWARE_VERSION "(v[^"]+)"', config).group(1)
        for name in (f"{version}.md", f"{version}.ja.md", f"{version}.en.md"):
            path = NOTES / name
            self.assertTrue(path.exists(), name)
            self.assertTrue(path.read_text(encoding="utf-8").startswith(f"# {version}\n"), name)

    def test_translations_always_have_an_original(self):
        for path in NOTES.glob("v*.*.md"):
            match = re.match(r"^(v.+)\.(ja|en)\.md$", path.name)
            if match:
                self.assertTrue((NOTES / f"{match.group(1)}.md").exists(), path.name)


if __name__ == "__main__":
    unittest.main()
