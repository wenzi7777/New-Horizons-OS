"""OTA downloads verify the server certificate (OtaTls.h), never setInsecure."""
import unittest
from pathlib import Path
import sys as _fsys
_fsys.path.insert(0, str(__import__('pathlib').Path(__file__).resolve().parent))
from _firmware import CORE_ROOT, firmware_file, firmware_sources, read_firmware  # noqa: E402

ROOT = Path(__file__).resolve().parents[1]
ARDUINO_ROOT = ROOT / "firmware" / "newhorizons_os"


class OtaTlsTests(unittest.TestCase):
    def test_no_insecure_tls_anywhere_in_the_firmware(self):
        for path in firmware_sources():
            self.assertNotIn("setInsecure()", path.read_text(encoding="utf-8"), path.name)

    def test_ota_clients_use_the_public_ca_bundle(self):
        ota = read_firmware("OtaManager.cpp")
        self.assertEqual(ota.count("trustPublicCertificateAuthorities(client);"), 2)
        tls = read_firmware("OtaTls.h")
        self.assertIn('asm("_binary_x509_crt_bundle_start")', tls)
        self.assertIn("client.setCACertBundle(", tls)


if __name__ == "__main__":
    unittest.main()
