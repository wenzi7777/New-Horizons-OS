"""Update channels (stable / beta): the firmware side.

Source-level checks, like the rest of tests/: the behaviour itself is in
VersionCompare.cpp / UpdateChannel.h, which tests_native runs for real.
"""
import re
import unittest
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[1]
ARDUINO_ROOT = REPO_ROOT / "firmware" / "newhorizons_os"


def read(name: str) -> str:
    return (ARDUINO_ROOT / name).read_text(encoding="utf-8")


class UpdateChannelTests(unittest.TestCase):
    def test_every_board_declares_a_manifest_base_without_suffix(self):
        board = read("BoardConfig.h")
        bases = re.findall(r'#define NHOS_BOARD_OTA_MANIFEST_BASE \\\n\s+"([^"]+)"', board)
        self.assertEqual(len(bases), 5, bases)
        for base in bases:
            self.assertFalse(base.endswith(".json"), base)
        self.assertNotIn("NHOS_BOARD_DEFAULT_OTA_MANIFEST_URL", board)

    def test_stable_manifest_keeps_the_legacy_latest_name(self):
        # Firmware older than the channels only ever reads <base>-latest.json,
        # so that name has to stay the stable manifest.
        config = read("Config.h")
        self.assertIn('kDefaultUpdateManifestUrl[] = NHOS_BOARD_OTA_MANIFEST_BASE "-latest.json"', config)
        channel = read("UpdateChannel.h")
        self.assertIn('return "-latest.json";', channel)
        self.assertIn('return "-beta.json";', channel)

    def test_ota_config_carries_channel_and_migrates_legacy_url(self):
        header = read("DeviceConfig.h")
        impl = read("DeviceConfig.cpp")
        self.assertIn("String channel = kUpdateChannelStable;", header)
        self.assertIn("bool setOtaConfig(bool autoApplyOnBoot, const String& manifestUrl, const String& channel);", header)
        self.assertIn("String resolvedManifestUrl() const;", header)
        # A stored stable default becomes "follow the channel".
        self.assertIn("manifestUrl == kDefaultUpdateManifestUrl ? String() : manifestUrl", impl)
        self.assertIn('"channel"', impl)
        self.assertIn('\\"resolved_manifest_url\\"', impl)

    def test_ota_paths_use_the_resolved_manifest(self):
        sketch = read("newhorizons_os.ino")
        self.assertIn("ota.autoApplyIfNewer(deviceConfig.resolvedManifestUrl())", sketch)
        receiver = read("EspNowOtaReceiver.cpp")
        self.assertNotIn("deviceConfig_->data().ota.manifestUrl", receiver)
        self.assertIn("deviceConfig_->resolvedManifestUrl()", receiver)
        server = read("ControlServer.cpp")
        self.assertNotIn('extractString(request, "manifest_url"));', server)
        self.assertGreaterEqual(server.count("requestedManifestUrl(request)"), 4)

    def test_set_ota_config_accepts_channel(self):
        server = read("ControlServer.cpp")
        start = server.index('if (cmd == "set_ota_config")')
        body = server[start:server.index('if (cmd == "check_update")', start)]
        self.assertIn('jsonExtractString(request, "channel", channel);', body)
        self.assertIn('"invalid_update_channel"', body)
        self.assertIn('\\"ota_channels\\":true', server)
        self.assertIn('"firmware_prerelease"', server)

    def test_config_registry_exposes_channel(self):
        registry = read("ConfigRegistry.cpp")
        self.assertIn('{"ota.channel", ConfigType::Enum, "set_ota_config", "channel", 0, 0, kUpdateChannelNames,', registry)
        self.assertIn('if (path == "ota.channel")', registry)

    def test_version_compares_are_prerelease_aware(self):
        for name in ("OtaManager.cpp", "EspNowOtaReceiver.cpp"):
            source = read(name)
            self.assertNotIn('sscanf(remote.c_str(), "v%d.%d.%d"', source, name)
            self.assertIn("compareVersions(remote.c_str(), local.c_str())", source, name)
        self.assertIn("compareVersionCores(kFirmwareVersion, minOs) >= 0", read("AppPackage.cpp"))

    def test_native_tests_cover_versions_and_channels(self):
        run = (REPO_ROOT / "tests_native" / "run.sh").read_text(encoding="utf-8")
        self.assertIn("test_version_compare", run)
        self.assertIn("spec/fixtures/versions.json", run)
        self.assertIn("test_update_channel", run)


if __name__ == "__main__":
    unittest.main()

