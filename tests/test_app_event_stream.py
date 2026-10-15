"""App events ride the stream packets (spec/PROTOCOL.md 3.1).

They used to reach the Desktop only by polling `app_events` twice a second,
which through a Hub competed with every other command for its single relay
slot. Now each stream packet carries the events raised since the last one, as
app_event extension TLVs; `app_events` remains for filling gaps.
"""
import json
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SKETCH = ROOT / "firmware" / "newhorizons_os"
CORE = ROOT / "firmware" / "libraries" / "nhos_core" / "src"


class AppEventStreamTests(unittest.TestCase):
    def test_the_wire_spec_defines_the_tlv(self):
        packet = json.loads((ROOT / "spec" / "wire.json").read_text(encoding="utf-8"))["packet"]
        self.assertEqual(packet["extension_types"]["app_event"], "0x01")
        self.assertEqual((packet["app_event"]["max_app"], packet["app_event"]["max_event"], packet["app_event"]["max_detail"]), (15, 23, 47))
        self.assertIn("app_event", (ROOT / "spec" / "PROTOCOL.md").read_text(encoding="utf-8"))

    def test_the_string_limits_match_the_event_record(self):
        # EventRecord's char arrays hold one byte less than their size.
        header = (SKETCH / "AppManager.h").read_text(encoding="utf-8")
        self.assertIn("char app[16]", header)
        self.assertIn("char event[24]", header)
        self.assertIn("char detail[48]", header)

    def test_every_stream_packet_takes_the_pending_events(self):
        sketch = (SKETCH / "newhorizons_os.ino").read_text(encoding="utf-8")
        scan = sketch[sketch.index("void scanAndStreamIfDue()"):sketch.index("void sendQueuedPacketIfAny()")]
        self.assertLess(scan.index("apps.takeStreamEvents("), scan.index("packetBuilder.buildMatrixPacketHeader("))
        self.assertIn("eventBytes ? eventExtensions : nullptr, eventBytes", scan)

    def test_a_full_packet_leaves_the_rest_for_the_next(self):
        source = (SKETCH / "AppManager.cpp").read_text(encoding="utf-8")
        take = source[source.index("size_t AppManager::takeStreamEvents("):source.index("String AppManager::eventsJson(")]
        self.assertIn("encodeAppEventTlv(", take)
        self.assertIn("break;  // full", take)
        self.assertIn("streamedSeq_ = record.seq;", take)

    def test_app_events_says_the_device_streams_them(self):
        source = (SKETCH / "AppManager.cpp").read_text(encoding="utf-8")
        self.assertIn('\\"streaming\\":true', source)

    def test_the_codec_writes_extensions_after_the_battery_block(self):
        codec = (CORE / "PacketV5Codec.cpp").read_text(encoding="utf-8")
        trailing = codec[codec.index("size_t writeTrailingBlocks("):codec.index("}  // namespace\n")]
        self.assertLess(trailing.index("input.battery->socCentiPercent"), trailing.index("input.extensions, input.extensionBytes"))
        self.assertIn("kPacketFlagExtensions", codec)


if __name__ == "__main__":
    unittest.main()
