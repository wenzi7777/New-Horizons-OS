import importlib.util
import struct
import unittest
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[1]


def load_wifi_portal():
    path = REPO_ROOT / "device" / "recovery" / "wifi_portal.py"
    spec = importlib.util.spec_from_file_location("wifi_portal_test", path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def build_dns_query(name):
    parts = name.split(".")
    qname = b"".join(bytes([len(part)]) + part.encode("ascii") for part in parts) + b"\x00"
    return (
        b"\x12\x34"  # transaction id
        + b"\x01\x00"  # standard query
        + b"\x00\x01"  # qdcount
        + b"\x00\x00"  # ancount
        + b"\x00\x00"  # nscount
        + b"\x00\x00"  # arcount
        + qname
        + b"\x00\x01"  # qtype A
        + b"\x00\x01"  # qclass IN
    )


class WiFiPortalCaptiveTests(unittest.TestCase):
    def test_normalize_path_handles_absolute_probe_url(self):
        module = load_wifi_portal()
        normalized = module._normalize_path("http://connectivitycheck.gstatic.com/generate_204?x=1")
        self.assertEqual(normalized, "/generate_204")

    def test_dns_server_responds_with_portal_ip(self):
        module = load_wifi_portal()
        dns = module.CaptiveDnsServer("192.168.4.1")
        packet = build_dns_query("example.com")

        response = dns._build_response(packet)

        self.assertTrue(response.startswith(b"\x12\x34\x81\x80"))
        self.assertIn(b"\xc0\x0c\x00\x01\x00\x01", response)
        self.assertTrue(response.endswith(bytes([192, 168, 4, 1])))


if __name__ == "__main__":
    unittest.main()
