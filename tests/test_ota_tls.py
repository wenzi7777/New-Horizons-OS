"""OTA downloads verify the server certificate (OtaTls.h), never setInsecure."""
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
ARDUINO_ROOT = ROOT / "firmware" / "newhorizons_os"


class OtaTlsTests(unittest.TestCase):
    def test_no_insecure_tls_anywhere_in_the_firmware(self):
        for path in sorted(ARDUINO_ROOT.glob("*.cpp")) + sorted(ARDUINO_ROOT.glob("*.h")) + [ARDUINO_ROOT / "newhorizons_os.ino"]:
            self.assertNotIn("setInsecure()", path.read_text(encoding="utf-8"), path.name)

    def test_ota_clients_use_the_public_ca_bundle(self):
        ota = (ARDUINO_ROOT / "OtaManager.cpp").read_text(encoding="utf-8")
        self.assertEqual(ota.count("trustPublicCertificateAuthorities(client);"), 2)
        tls = (ARDUINO_ROOT / "OtaTls.h").read_text(encoding="utf-8")
        self.assertIn('asm("_binary_x509_crt_bundle_start")', tls)
        self.assertIn("client.setCACertBundle(", tls)


if __name__ == "__main__":
    unittest.main()
