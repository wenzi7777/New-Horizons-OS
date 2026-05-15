import storage


class ManifestPlanner:
    def __init__(self, root_dir="."):
        self.root_dir = root_dir

    def plan(self, manifest):
        downloads = []
        reboot_required = False
        for entry in manifest.get("files", []):
            path = entry.get("path", "")
            if not path:
                continue
            local_path = self._local_path(path)
            current_hash = storage.sha256_hex_file(local_path)
            if current_hash == entry.get("sha256"):
                continue
            item = {
                "path": path,
                "url": self._make_url(manifest.get("base_url", ""), path),
                "sha256": entry.get("sha256", ""),
                "size": entry.get("size", 0),
                "kind": entry.get("kind", "code"),
                "reboot_required": bool(entry.get("reboot_required", False)),
            }
            downloads.append(item)
            if item["reboot_required"]:
                reboot_required = True
        return {"downloads": downloads, "reboot_required": reboot_required}

    def verify_download(self, expected_sha256, payload):
        return storage.sha256_hex_bytes(payload) == expected_sha256

    def apply_file(self, relative_path, payload):
        storage.write_bytes(self._local_path(relative_path), payload)

    def _local_path(self, relative_path):
        relative_path = relative_path.lstrip("/")
        return relative_path if self.root_dir in ("", ".") else self.root_dir.rstrip("/") + "/" + relative_path

    def _make_url(self, base_url, path):
        return path if not base_url else base_url.rstrip("/") + "/" + path.lstrip("/")
