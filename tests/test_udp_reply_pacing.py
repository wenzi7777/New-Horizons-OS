"""Large UDP command results are queued and paced, not sent in one burst.

The blocking loop (one result_chunk after another with delay(3)) held the
main loop ~6 ms per chunk -- ~116 ms for a `status` reply. See ReplyPacing.h.
"""
import unittest
from pathlib import Path

import sys as _sys
_sys.path.insert(0, str(__import__('pathlib').Path(__file__).resolve().parent))
from _control_source import command_names, control_source, flagged, handler_body  # noqa: E402

ARDUINO_ROOT = Path(__file__).resolve().parents[1] / "firmware" / "newhorizons_os"


def body(text: str, signature: str) -> str:
    start = text.index(signature)
    return text[start:text.index("\n}\n", start)]


class UdpReplyPacingTests(unittest.TestCase):
    def setUp(self):
        self.impl = control_source()
        self.header = (ARDUINO_ROOT / "ControlServer.h").read_text(encoding="utf-8")

    def test_large_results_are_queued(self):
        send = body(self.impl, "void ControlServer::sendUdpResult(")
        self.assertIn("udpReplies_[udpReplyCount_++] = std::move(reply);", send)
        # The blocking loop survives only as the queue-full fallback.
        self.assertEqual(send.count("delay(3)"), 1)
        self.assertLess(send.index("udpReplyCount_ < kMaxPendingUdpReplies"), send.index("delay(3)"))

    def test_one_paced_chunk_per_tick(self):
        service = body(self.impl, "void ControlServer::serviceUdpReplies(")
        self.assertIn("ReplyPacing::maySend(nowUs, udpChunkReadyUs_, scanning, usUntilScan)", service)
        self.assertEqual(service.count("sendUdpChunk("), 1)
        self.assertIn("scanner_->nextScanDueUs()", service)

    def test_packets_are_still_read_while_a_reply_is_pending(self):
        command = body(self.impl, "void ControlServer::serviceUdpCommand(")
        # Servicing the queue must not return early: TIME_RESP shares the socket.
        self.assertLess(command.index("serviceUdpReplies(udp);"), command.index("udp.parsePacket()"))
        self.assertNotIn("if (udpReplyCount_", command)

    def test_wire_format_unchanged(self):
        chunk = body(self.impl, "void ControlServer::sendUdpChunk(")
        for field in ('"type", "result_chunk"', '"device_uid"', '"request_id"', '"chunk"', '"chunks"', '"data"'):
            self.assertIn(field, chunk)
        self.assertIn("constexpr size_t kUdpInlineLimit = 1200;", self.impl)
        self.assertIn("constexpr size_t kUdpChunkRaw = 700;", self.impl)


if __name__ == "__main__":
    unittest.main()


class HeavyCommandDeferralTests(unittest.TestCase):
    """status / storage_status over UDP wait for the gap after a scan."""

    def setUp(self):
        from _control_source import control_source
        self.source = control_source()

    def body(self, signature):
        start = self.source.index(signature)
        return self.source[start:self.source.index("\n}\n", start)]

    def test_heavy_commands_are_deferred_after_the_ack(self):
        command = self.body("void ControlServer::serviceUdpCommand(WiFiUDP& udp) {")
        # The ack goes out first, so the Gateway does not retry while it waits.
        self.assertLess(command.index("udp.print(ack);"), command.index("(spec->flags & kCmdHeavy) != 0"))
        self.assertIn("!heavyCommandMayRun(command.sinceUs)", command)
        self.assertIn("deferredUdp_ = std::move(command);", command)
        # The deferred one is serviced every tick, before new packets are read.
        self.assertLess(command.index("serviceDeferredUdpCommand(udp);"), command.index("udp.parsePacket()"))

    def test_no_scan_coming_means_no_wait(self):
        may_run = self.body("bool ControlServer::heavyCommandMayRun(uint32_t deferredSinceUs) const {")
        # The stream gate is part of "scanning": with it closed (maintenance,
        # WiFi down, an ESP-NOW reply pending) no scan comes to wait for.
        self.assertIn("(scanGate_ == nullptr || scanGate_())", may_run)
        self.assertIn("HeavyCommandPacing::mayRun(", may_run)
        sketch = (ARDUINO_ROOT / "newhorizons_os.ino").read_text(encoding="utf-8")
        self.assertIn("control.setScanGate(&streamingGateOk);", sketch)

    def test_only_udp_defers(self):
        # ESP-NOW: the gate is closed while a command is pending, so waiting
        # would only ever hit the bound. TCP: the client is per-call.
        esp = self.body("String ControlServer::serviceEspNowCommand(")
        self.assertNotIn("heavyCommandMayRun", esp)
        tcp = self.body("void ControlServer::service() {")
        self.assertNotIn("heavyCommandMayRun", tcp)
