"""app_install refuses a package without a digest to check it against."""
import unittest
from pathlib import Path

ARDUINO_ROOT = Path(__file__).resolve().parents[1] / "firmware" / "newhorizons_os"


def read(name: str) -> str:
    return (ARDUINO_ROOT / name).read_text(encoding="utf-8")


class AppInstallDigestTests(unittest.TestCase):
    def test_app_install_requires_a_sha256(self):
        registry = read("AppRegistry.cpp")
        start = registry.index("bool AppRegistry::install(")
        body = registry[start:registry.index("AppPackageManifest manifest;", start)]
        self.assertIn('error = "sha256_required";', body)
        self.assertIn("expectedSha256.length() != 64", body)
        self.assertNotIn("expectedSha256.length() > 0 &&", body)


if __name__ == "__main__":
    unittest.main()
