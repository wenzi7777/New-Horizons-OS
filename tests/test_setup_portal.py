"""The SoftAP setup page: generated asset in sync, three complete languages,
and nothing in the page that the simple minifier would break."""
from __future__ import annotations

import re
import subprocess
import sys
import unittest
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]
PAGE = REPO / "firmware" / "libraries" / "nhos_core" / "portal" / "setup_portal.html"
GEN = REPO / "firmware" / "scripts" / "gen_portal_asset.py"


def language_keys(page: str) -> dict[str, set[str]]:
    keys = {}
    for lang in ("en", "zh", "ja"):
        match = re.search(rf"\n{lang}:\{{(.*?)\}}(?:,\n|\}};)", page, re.S)
        assert match, lang
        keys[lang] = set(re.findall(r'([A-Za-z_]+):"', match.group(1)))
    return keys


class SetupPortalPageTests(unittest.TestCase):
    def setUp(self):
        self.page = PAGE.read_text(encoding="utf-8")

    def test_generated_asset_is_up_to_date(self):
        result = subprocess.run([sys.executable, str(GEN), "--check"], capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stderr)

    def test_every_language_has_every_string(self):
        keys = language_keys(self.page)
        self.assertGreater(len(keys["en"]), 40)
        self.assertEqual(keys["zh"], keys["en"])
        self.assertEqual(keys["ja"], keys["en"])

    def test_every_data_t_key_is_translated(self):
        used = set(re.findall(r'data-t="([A-Za-z_]+)"', self.page))
        self.assertTrue(used <= language_keys(self.page)["en"], used - language_keys(self.page)["en"])

    def test_every_status_and_error_code_is_translated(self):
        en = language_keys(self.page)["en"]
        # SetupPortal / the products report these; the page shows t(code).
        for code in ("required", "bad_url", "wrong_password", "not_found", "timeout", "other_fail",
                     "busy", "backend_unreachable"):
            self.assertIn(code, en)

    def test_minifier_safe(self):
        # gen_portal_asset.py joins stripped lines: a // comment would swallow
        # the rest of the script.
        for line in self.page.splitlines():
            self.assertFalse(line.strip().startswith("//"), line)

    def test_free_text_fields_skip_autocorrect(self):
        for field in ("ssid", "url", "token"):
            tag = re.search(rf'<input id="{field}"[^>]*>', self.page).group(0)
            self.assertIn('autocapitalize="none"', tag)
            self.assertIn('autocorrect="off"', tag)
            self.assertIn('spellcheck="false"', tag)

    def test_never_shows_a_saved_secret(self):
        portal = (REPO / "firmware" / "libraries" / "nhos_core" / "src" / "SetupPortal.cpp").read_text()
        self.assertNotIn('"password"', portal)
        self.assertNotIn("auth_token", portal)


if __name__ == "__main__":
    unittest.main()
