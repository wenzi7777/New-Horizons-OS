"""Update channels (stable / beta): the firmware side.

Source-level checks, like the rest of tests/: the behaviour itself is in
VersionCompare.cpp / UpdateChannel.h, which tests_native runs for real.
"""
import re
import unittest
from pathlib import Path

import sys as _sys
_sys.path.insert(0, str(__import__('pathlib').Path(__file__).resolve().parent))
from _control_source import command_names, control_source, flagged, handler_body  # noqa: E402
import sys as _fsys
_fsys.path.insert(0, str(__import__('pathlib').Path(__file__).resolve().parent))
from _firmware import CORE_ROOT, firmware_file, firmware_sources, read_firmware  # noqa: E402

REPO_ROOT = Path(__file__).resolve().parents[1]
ARDUINO_ROOT = REPO_ROOT / "firmware" / "newhorizons_os"


def read(name: str) -> str:
    return read_firmware(name)


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
        server = control_source()
        self.assertNotIn('extractString(request, "manifest_url"));', server)
        self.assertGreaterEqual(server.count("requestedManifestUrl(request)"), 4)

    def test_set_ota_config_accepts_channel(self):
        server = control_source()
        body = handler_body("set_ota_config")
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




class DirectModeCheckTests(unittest.TestCase):
    def test_a_direct_mode_check_reports_the_offered_version_when_current(self):
        # Over ESP-NOW a "you are on the latest" check used to report an
        # empty version and URL, unlike OtaManager over WiFi.
        impl = read("EspNowOtaReceiver.cpp")
        reply = impl[impl.index("void EspNowOtaReceiver::handleManifestReply"):]
        reply = reply[:reply.index("\n}\n")]
        assigned = reply.index("pendingVersion_ = version;")
        self.assertLess(assigned, reply.index('lastResult_ = "current";'))
        self.assertLess(reply.index("pendingUrl_ = url;"), reply.index('lastResult_ = "current";'))
        # Availability alone decides whether a relay starts.
        self.assertIn("updateAvailable_ = false;", reply[assigned:])


if __name__ == "__main__":
    unittest.main()
