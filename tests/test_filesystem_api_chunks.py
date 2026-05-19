import hashlib
import importlib.util
import sys
import tempfile
import unittest
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[1]


def load_module(module_path: Path, module_name: str):
    sys.modules.pop("storage", None)
    recovery_path = REPO_ROOT / "device" / "recovery"
    sys.path.insert(0, str(recovery_path))
    sys.path.insert(0, str(module_path.parent))
    try:
        spec = importlib.util.spec_from_file_location(module_name, module_path)
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
        return module
    finally:
        sys.path.pop(0)
        sys.path.pop(0)


class FilesystemAPIChunkTests(unittest.TestCase):
    MODULES = (
        (
            REPO_ROOT / "device" / "recovery" / "filesystem_api.py",
            "minimal_filesystem_api_chunks_test",
        ),
        (
            REPO_ROOT / "device" / "os" / "filesystem_api.py",
            "full_filesystem_api_chunks_test",
        ),
    )

    def test_upload_and_download_are_chunk_based_and_hash_checked(self):
        payload = b"0123456789abcdef" * 512
        expected_sha = hashlib.sha256(payload).hexdigest()
        for module_path, module_name in self.MODULES:
            with self.subTest(module=module_name), tempfile.TemporaryDirectory() as tmpdir:
                module = load_module(module_path, module_name)
                api = module.FilesystemAPI(root=str(Path(tmpdir) / "files"), tmp_root=str(Path(tmpdir) / "tmp"))

                begin = api.upload_begin("captures/sample.bin", len(payload), expected_sha)
                self.assertEqual(begin["status"], "ok")

                first = payload[:1024]
                second = payload[1024:]
                self.assertEqual(api.upload_chunk("captures/sample.bin", 0, first.hex())["written"], len(first))
                self.assertEqual(api.upload_chunk("captures/sample.bin", len(first), second.hex())["written"], len(payload))

                finished = api.upload_finish("captures/sample.bin")
                self.assertEqual(finished["status"], "ok")
                self.assertEqual(finished["sha256"], expected_sha)

                download = api.download_begin("captures/sample.bin")
                self.assertEqual(download["size"], len(payload))
                self.assertEqual(download["sha256"], expected_sha)
                first_chunk = api.download_chunk("captures/sample.bin", 0, 512)
                self.assertEqual(bytes.fromhex(first_chunk["data"]), payload[:512])
                self.assertTrue(first_chunk["has_more"])

    def test_file_api_rejects_path_escape(self):
        module = load_module(
            REPO_ROOT / "device" / "os" / "filesystem_api.py",
            "full_filesystem_api_escape_test",
        )
        with tempfile.TemporaryDirectory() as tmpdir:
            api = module.FilesystemAPI(root=str(Path(tmpdir) / "files"), tmp_root=str(Path(tmpdir) / "tmp"))
            with self.assertRaises(ValueError):
                api.download_begin("../os/main.py")

    def test_scopes_map_to_separate_roots_and_default_to_user(self):
        module = load_module(
            REPO_ROOT / "device" / "os" / "filesystem_api.py",
            "full_filesystem_api_scope_test",
        )
        with tempfile.TemporaryDirectory() as tmpdir:
            root = Path(tmpdir)
            api = module.FilesystemAPI(
                root=str(root / "files"),
                tmp_root=str(root / "tmp"),
                scope_roots={
                    "user": str(root / "files"),
                    "logs": str(root / "logs"),
                    "calibration": str(root / "calibration"),
                },
            )

            api.upload_begin("user.json", 2, "", scope="user")
            api.upload_chunk("user.json", 0, "7b7d", scope="user")
            api.upload_finish("user.json", scope="user")
            (root / "logs").mkdir()
            (root / "logs" / "device.log").write_text("hello", encoding="utf-8")
            (root / "calibration").mkdir()
            (root / "calibration" / "level.json").write_text("{}", encoding="utf-8")

            self.assertEqual(api.download_begin("user.json")["scope"], "user")
            self.assertEqual(api.download_begin("device.log", scope="logs")["scope"], "logs")
            self.assertEqual(api.download_begin("level.json", scope="calibration")["scope"], "calibration")
            self.assertTrue(any(item["scope"] == "logs" for item in api.list_files(scope="logs")))

            with self.assertRaises(ValueError):
                api.list_files(scope="system")

    def test_usage_reports_scoped_storage_bytes(self):
        for module_path, module_name in self.MODULES:
            with self.subTest(module=module_name), tempfile.TemporaryDirectory() as tmpdir:
                module = load_module(module_path, module_name)
                root = Path(tmpdir)
                api = module.FilesystemAPI(
                    root=str(root / "files"),
                    tmp_root=str(root / "tmp"),
                    scope_roots={
                        "user": str(root / "files"),
                        "logs": str(root / "logs"),
                        "calibration": str(root / "calibration"),
                    },
                )
                (root / "files").mkdir()
                (root / "logs").mkdir()
                (root / "calibration").mkdir()
                (root / "files" / "profile.json").write_bytes(b"{}")
                (root / "logs" / "device.log").write_bytes(b"hello")
                (root / "calibration" / "level.json").write_bytes(b"1234")

                usage = api.usage()

                self.assertGreaterEqual(usage["total_bytes"], usage["used_bytes"])
                self.assertEqual(usage["scopes"]["user"], 2)
                self.assertEqual(usage["scopes"]["logs"], 5)
                self.assertEqual(usage["scopes"]["calibration"], 4)
                self.assertIn("other_bytes", usage)


if __name__ == "__main__":
    unittest.main()
