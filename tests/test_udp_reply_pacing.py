"""Large UDP command results are queued and paced, not sent in one burst.

The blocking loop (one result_chunk after another with delay(3)) held the
main loop ~6 ms per chunk -- ~116 ms for a `status` reply. See ReplyPacing.h.
"""
import unittest
from pathlib import Path

ARDUINO_ROOT = Path(__file__).resolve().parents[1] / "firmware" / "newhorizons_os"


def body(text: str, signature: str) -> str:
    start = text.index(signature)
    return text[start:text.index("\n}\n", start)]


class UdpReplyPacingTests(unittest.TestCase):
    def setUp(self):
        self.impl = (ARDUINO_ROOT / "ControlServer.cpp").read_text(encoding="utf-8")
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
