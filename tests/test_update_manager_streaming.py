import importlib.util
import sys
import tempfile
import unittest
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[1]


def load_module(module_path: Path, module_name: str):
    sys.modules.pop("storage", None)
    sys.modules.pop("manifest_update", None)
    sys.path.insert(0, str(module_path.parent))
    try:
        spec = importlib.util.spec_from_file_location(module_name, module_path)
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
        return module
    finally:
        sys.path.pop(0)


class FakeConfigStore:
    def __init__(self):
        self.runtime = {
            "update": {
                "enabled": True,
                "manifest_url": "https://example.com/manifest.json",
            }
        }
        self.update_state = {
            "last_manifest_sha256": "",
            "last_check_ms": 0,
            "last_result": "",
            "reboot_required": False,
        }

    def load_runtime(self):
        return dict(self.runtime)

    def load_update_state(self):
        return dict(self.update_state)

    def save_update_state(self, state):
        self.update_state = dict(state)


class FakePlanner:
    def __init__(self, root_dir):
        self.root_dir = Path(root_dir)

    def plan(self, manifest):
        return {
            "downloads": [
                {
                    "path": "big.py",
                    "url": "https://example.com/big.py",
                    "sha256": manifest["sha256"],
                    "size": len(manifest["payload"]),
                    "kind": "code",
                    "reboot_required": True,
                }
            ],
            "reboot_required": True,
        }

    def _local_path(self, relative_path):
        return str(self.root_dir / relative_path)


class FakeRaw:
    def __init__(self, payload, chunk_size):
        self.payload = payload
        self.chunk_size = chunk_size
        self.offset = 0

    def read(self, size=-1):
        if self.offset >= len(self.payload):
            return b""
        if size is None or size < 0:
            size = self.chunk_size
        end = min(len(self.payload), self.offset + min(size, self.chunk_size))
        chunk = self.payload[self.offset:end]
        self.offset = end
        return chunk


class FakeResponse:
    def __init__(self, payload, chunk_size):
        self.raw = FakeRaw(payload, chunk_size)
        self.closed = False

    @property
    def content(self):
        raise MemoryError("content property should not be used for large downloads")

    def close(self):
        self.closed = True


class UpdateManagerStreamingTests(unittest.TestCase):
    def _assert_streaming_download(self, module_path: Path, module_name: str):
        module = load_module(module_path, module_name)
        payload = (b"0123456789abcdef" * 2048)
        chunk_size = 1024
        with tempfile.TemporaryDirectory() as tmpdir:
            store = FakeConfigStore()
            planner = FakePlanner(tmpdir)
            manager = module.UpdateManager(store, logger=None, root_dir=tmpdir)
            manager.planner = planner
            manager._fetch_json = lambda url: {
                "payload": payload,
                "sha256": module.storage.sha256_hex_bytes(payload),
            }

            module.requests = type(
                "FakeRequests",
                (),
                {"get": staticmethod(lambda url: FakeResponse(payload, chunk_size))},
            )
            result = manager.apply()
            self.assertEqual(result["status"], "ok")
            self.assertEqual(result["message"], "update_applied")
            self.assertEqual(module.storage.read_bytes(str(Path(tmpdir) / "big.py")), payload)

    def test_os_update_manager_streams_large_downloads(self):
        self._assert_streaming_download(
            REPO_ROOT / "device" / "os" / "update_manager.py",
            "os_update_manager_streaming_test",
        )


if __name__ == "__main__":
    unittest.main()
