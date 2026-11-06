"""ESP-NOW heartbeat: a device that is not scanning stays known to its Hub.

The Hub learns a device's UID only from the NHO header of a data frame.
A device that boots into maintenance mode (or has no layout) sends none, so
without a heartbeat it never reaches the Backend's routing table.
"""
import sys as _sys
import unittest
from pathlib import Path

_sys.path.insert(0, str(Path(__file__).resolve().parent))
from _firmware import read_firmware  # noqa: E402


def heartbeat_body() -> str:
    sketch = read_firmware("newhorizons_os.ino")
    body = sketch[sketch.index("void sendHeartbeatIfDue() {"):]
    return body[:body.index("\n}\n")]


class EspNowHeartbeatTests(unittest.TestCase):
    def test_espnow_sends_only_while_paired_and_not_scanning(self):
        body = heartbeat_body()
        self.assertIn("if (espNowMode) {", body)
        self.assertIn("if (!espNowTransport.ready() || scanner.active()) {", body)
        self.assertIn("} else if (!wifi.isConnected() || !findme.hasGateway()) {", body)

    def test_heartbeat_task_is_registered_in_espnow_mode(self):
        sketch = read_firmware("newhorizons_os.ino")
        tasks = sketch[sketch.index("void registerRuntimeTasks() {"):]
        espnow = tasks[tasks.index("if (espNowMode) {"):tasks.index("} else {")]
        self.assertIn('scheduler.registerTask("heartbeat", &sendHeartbeatIfDue, false);', espnow)


if __name__ == "__main__":
    unittest.main()
