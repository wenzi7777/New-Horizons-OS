"""ESP-NOW channel scan reaches every channel a Hub can be on.

The radio starts in ESP-IDF's world-safe country "01" (channels 1-11). A
device in ESP-NOW mode never joins an AP, so nothing widens that: without an
explicit country, hops to 12 and 13 fail and a Hub behind an AP there is
never found.
"""
import sys as _sys
import unittest
from pathlib import Path

_sys.path.insert(0, str(Path(__file__).resolve().parent))
from _firmware import read_firmware  # noqa: E402


def begin_body() -> str:
    source = read_firmware("EspNowPairing.cpp")
    body = source[source.index("bool EspNowPairing::begin("):]
    return body[:body.index("\n}\n")]


class EspNowChannelScanTests(unittest.TestCase):
    def test_country_is_set_before_espnow_starts(self):
        body = begin_body()
        self.assertIn('esp_wifi_set_country_code("JP", false)', body)
        self.assertLess(body.index("esp_wifi_set_country_code"), body.index("esp_now_init()"))

    def test_scan_list_still_covers_channels_1_to_13(self):
        header = read_firmware("EspNowPairing.h")
        self.assertIn("kEspNowScanChannels[] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13};", header)

    def test_refused_channel_switch_is_logged(self):
        source = read_firmware("EspNowPairing.cpp")
        hop = source[source.index("void EspNowPairing::startScanIfNeeded() {"):]
        hop = hop[:hop.index("\n}\n")]
        self.assertIn("const esp_err_t chanErr = esp_wifi_set_channel(", hop)
        self.assertIn("esp_wifi_set_channel(%u) -> %d", hop)


if __name__ == "__main__":
    unittest.main()
