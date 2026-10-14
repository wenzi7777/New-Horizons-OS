"""NVS key names fit ESP-IDF's 15-character limit.

A longer key does not fail to compile or crash: Preferences just logs
KEY_TOO_LONG and the write never happens, so the value silently reverts on
the next boot. That is how the manual battery profile and the SPIFFS
first-boot flag were lost on every reboot until v1.14.0 -- and builds with
CORE_DEBUG_LEVEL=0 do not even print the error. This scans every key literal
handed to Preferences / Storage, in this firmware and in the Hub beside it.
"""
import re
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SOURCES = [ROOT / "firmware" / "newhorizons_os"]
HUB = ROOT.parent / "New-Horizons-Hub" / "newhorizons_hub"
if HUB.exists():
    SOURCES.append(HUB)

NVS_MAX_KEY = 15
CALL = re.compile(
    r"\b(?:put|get)(?:String|UInt|UChar|Bool|Int|ULong|ULong64|Long|Long64|Short|UShort|Char|Float|Double|Bytes|BytesLength)"
    r'\(\s*"([^"]+)"'
)
KEY_CONSTANT = re.compile(r'constexpr char k\w*Key\w*\[\] = "([^"]+)"')
STRUCTURAL = re.compile(r'\b(?:isKey|remove)\(\s*"([^"]+)"')


def source_files():
    for directory in SOURCES:
        for pattern in ("*.cpp", "*.h", "*.ino"):
            yield from sorted(directory.glob(pattern))


class NvsKeyLengthTests(unittest.TestCase):
    def test_every_literal_key_fits(self):
        too_long = []
        found = 0
        for path in source_files():
            text = path.read_text(encoding="utf-8")
            for regex in (CALL, KEY_CONSTANT, STRUCTURAL):
                for match in regex.finditer(text):
                    key = match.group(1)
                    if key.startswith("/"):
                        continue  # a SPIFFS path through a same-named file helper
                    found += 1
                    if len(key) > NVS_MAX_KEY:
                        too_long.append(f"{path.parent.name}/{path.name}: {key} ({len(key)})")
        self.assertGreater(found, 20, "key scan matched nothing; regex out of date?")
        self.assertEqual(too_long, [])

    def test_dynamic_keys_stay_bounded(self):
        # Keys built at runtime: prefix + a bounded suffix.
        flow = (ROOT / "firmware" / "newhorizons_os" / "FlowApp.cpp").read_text(encoding="utf-8")
        self.assertIn('String key = "pc_";', flow)
        self.assertIn("substring(0, 12)", flow)  # 3 + 12 = 15
        apps = (ROOT / "firmware" / "newhorizons_os" / "AppManager.cpp").read_text(encoding="utf-8")
        self.assertIn('String("app_en_") + String(name)', apps)  # slot names are flow..flow3


if __name__ == "__main__":
    unittest.main()
