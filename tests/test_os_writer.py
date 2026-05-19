import hashlib
import importlib.util
import json
import sys
import tempfile
import unittest
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[1]
OS_WRITER_PATH = REPO_ROOT / "device" / "recovery" / "os_writer.py"


def load_os_writer_module():
    sys.modules.pop("storage", None)
    sys.path.insert(0, str(OS_WRITER_PATH.parent))
    try:
        spec = importlib.util.spec_from_file_location("os_writer_test_module", OS_WRITER_PATH)
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
        return module
    finally:
        sys.path.pop(0)


class FakeResponse:
    def __init__(self, payload):
        self.payload = payload
        self.offset = 0
        self.raw = self

    @property
    def content(self):
        return self.payload

    def read(self, size=-1):
        if self.offset >= len(self.payload):
            return b""
        if size is None or size < 0:
            size = len(self.payload) - self.offset
        end = min(len(self.payload), self.offset + size)
        chunk = self.payload[self.offset:end]
        self.offset = end
        return chunk

    def close(self):
        return None


class OSWriterTests(unittest.TestCase):
    def test_write_os_skips_matching_hashes_and_downloads_only_changed_files(self):
        module = load_os_writer_module()
        app_payload = b"print('already installed')\n"
        config_payload = b"VALUE = 2\n"
        manifest = {
            "manifest_version": 2,
            "type": "os",
            "version": "v0.2.0",
            "base_url": "https://example.com/os",
            "target_root": "/os",
            "files": [
                {
                    "path": "app.py",
                    "sha256": hashlib.sha256(app_payload).hexdigest(),
                    "size": len(app_payload),
                },
                {
                    "path": "config.py",
                    "sha256": hashlib.sha256(config_payload).hexdigest(),
                    "size": len(config_payload),
                },
            ],
        }
        release = {
            "product": "New Horizons OS",
            "latest": "v0.2.0",
            "manifest_url": "https://example.com/os-manifest.json",
        }
        urls = {
            "https://example.com/latest.json": json.dumps(release).encode(),
            "https://example.com/os-manifest.json": json.dumps(manifest).encode(),
            "https://example.com/os/config.py": config_payload,
        }
        requested = []

        def fake_get(url):
            requested.append(url)
            return FakeResponse(urls[url])

        module.requests = type("FakeRequests", (), {"get": staticmethod(fake_get)})

        with tempfile.TemporaryDirectory() as tmpdir:
            os_root = Path(tmpdir) / "os"
            os_root.mkdir()
            (os_root / "app.py").write_bytes(app_payload)
            writer = module.OSWriter(root_dir=tmpdir)

            result = writer.write_os("https://example.com/latest.json")

            self.assertEqual(result["status"], "ok")
            self.assertEqual(result["message"], "os_write_complete")
            self.assertEqual(result["downloaded_files"], 1)
            self.assertEqual(result["skipped_files"], 1)
            self.assertEqual((os_root / "config.py").read_bytes(), config_payload)
            self.assertEqual((Path(tmpdir) / "device_state" / "os_state.json").exists(), True)
            self.assertNotIn("https://example.com/os/app.py", requested)

            requested.clear()
            urls.pop("https://example.com/os/config.py")
            second = writer.write_os("https://example.com/latest.json")

            self.assertEqual(second["downloaded_files"], 0)
            self.assertEqual(second["skipped_files"], 2)
            self.assertEqual(
                requested,
                ["https://example.com/latest.json", "https://example.com/os-manifest.json"],
            )


if __name__ == "__main__":
    unittest.main()
