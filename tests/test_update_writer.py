import hashlib
import importlib.util
import json
import sys
import tempfile
import unittest
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[1]
UPDATE_WRITER_PATH = REPO_ROOT / "device" / "os" / "update_writer.py"


def load_update_writer_module():
    sys.modules.pop("storage", None)
    sys.path.insert(0, str(UPDATE_WRITER_PATH.parent))
    try:
        spec = importlib.util.spec_from_file_location("update_writer_test_module", UPDATE_WRITER_PATH)
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


class ManifestTargetWriterTests(unittest.TestCase):
    def test_write_recovery_uses_nested_release_and_recovery_target_root(self):
        module = load_update_writer_module()
        app_payload = b"RECOVERY_VERSION = 'v0.2.18'\n"
        manifest = {
            "manifest_version": 1,
            "type": "recovery",
            "version": "v0.2.18",
            "base_url": "https://example.com/recovery",
            "target_root": "/recovery",
            "files": [
                {
                    "path": "immutable_config.py",
                    "sha256": hashlib.sha256(app_payload).hexdigest(),
                    "size": len(app_payload),
                },
            ],
            "delete": ["old.py"],
        }
        release = {
            "product": "New Horizons OS",
            "latest": "v0.2.18",
            "manifest_url": "https://example.com/os-manifest.json",
            "recovery": {
                "version": "v0.2.18",
                "manifest_url": "https://example.com/recovery-manifest.json",
            },
        }
        urls = {
            "https://example.com/latest.json": json.dumps(release).encode(),
            "https://example.com/recovery-manifest.json": json.dumps(manifest).encode(),
            "https://example.com/recovery/immutable_config.py": app_payload,
        }
        requested = []

        def fake_get(url):
            requested.append(url)
            return FakeResponse(urls[url])

        module.requests = type("FakeRequests", (), {"get": staticmethod(fake_get)})

        with tempfile.TemporaryDirectory() as tmpdir:
            recovery_root = Path(tmpdir) / "recovery"
            recovery_root.mkdir()
            (recovery_root / "old.py").write_text("legacy\n", encoding="utf-8")
            writer = module.ManifestTargetWriter("recovery", root_dir=tmpdir)

            checked = writer.check_release("https://example.com/latest.json")
            result = writer.write_release("https://example.com/latest.json")

            self.assertEqual(checked["message"], "recovery_release_checked")
            self.assertEqual(checked["manifest_url"], "https://example.com/recovery-manifest.json")
            self.assertEqual(result["message"], "recovery_write_complete")
            self.assertEqual(result["version"], "v0.2.18")
            self.assertEqual((recovery_root / "immutable_config.py").read_bytes(), app_payload)
            self.assertFalse((recovery_root / "old.py").exists())
            self.assertEqual((Path(tmpdir) / "device_state" / "recovery_state.json").exists(), True)
            self.assertIn("https://example.com/recovery/immutable_config.py", requested)

    def test_rejects_manifest_with_wrong_target_root(self):
        module = load_update_writer_module()
        manifest = {
            "manifest_version": 1,
            "type": "os",
            "version": "v0.2.18",
            "base_url": "https://example.com/os",
            "target_root": "/nhos",
            "files": [{"path": "app.mpy", "sha256": "0" * 64, "size": 1}],
        }
        release = {
            "recovery": {
                "version": "v0.2.18",
                "manifest_url": "https://example.com/wrong.json",
            },
        }
        urls = {
            "https://example.com/latest.json": json.dumps(release).encode(),
            "https://example.com/wrong.json": json.dumps(manifest).encode(),
        }

        def fake_get(url):
            return FakeResponse(urls[url])

        module.requests = type("FakeRequests", (), {"get": staticmethod(fake_get)})

        with tempfile.TemporaryDirectory() as tmpdir:
            writer = module.ManifestTargetWriter("recovery", root_dir=tmpdir)
            with self.assertRaises(ValueError):
                writer.write_release("https://example.com/latest.json")


if __name__ == "__main__":
    unittest.main()
