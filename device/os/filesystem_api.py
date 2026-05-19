import storage
try:
    import ubinascii as binascii
except ImportError:
    import binascii


class FilesystemAPI:
    def __init__(self, root="data/files", tmp_root="data/tmp"):
        self.root = root
        self.tmp_root = tmp_root

    def _safe_rel(self, relative_path):
        path = str(relative_path or "").replace("\\", "/").strip("/")
        parts = [part for part in path.split("/") if part]
        if not parts or any(part in (".", "..") for part in parts):
            raise ValueError("invalid_path")
        return "/".join(parts)

    def _path(self, relative_path):
        return self.root.rstrip("/") + "/" + self._safe_rel(relative_path)

    def _tmp_path(self, relative_path):
        return self.tmp_root.rstrip("/") + "/" + self._safe_rel(relative_path) + ".upload"

    def _meta_path(self, relative_path):
        return self.tmp_root.rstrip("/") + "/" + self._safe_rel(relative_path) + ".upload.json"

    def list_files(self):
        return storage.list_tree(self.root)

    def read_file(self, relative_path):
        path = self._path(relative_path)
        text = storage.read_text(path)
        if text is not None:
            return {"path": relative_path, "encoding": "text", "content": text}
        data = storage.read_bytes(path)
        if data is None:
            return None
        return {"path": relative_path, "encoding": "hex", "content": data.hex()}

    def delete_file(self, relative_path):
        return storage.remove(self._path(relative_path))

    def upload_begin(self, relative_path, size, sha256=""):
        rel = self._safe_rel(relative_path)
        tmp_path = self._tmp_path(rel)
        meta_path = self._meta_path(rel)
        storage.ensure_dir(storage.dirname(tmp_path))
        storage.ensure_dir(storage.dirname(meta_path))
        storage.remove(tmp_path)
        storage.save_json(meta_path, {
            "path": rel,
            "size": int(size),
            "sha256": str(sha256 or ""),
            "written": 0,
        })
        with open(tmp_path, "wb"):
            pass
        return {"status": "ok", "message": "upload_started", "path": rel, "size": int(size)}

    def upload_chunk(self, relative_path, offset, data_hex):
        rel = self._safe_rel(relative_path)
        tmp_path = self._tmp_path(rel)
        meta_path = self._meta_path(rel)
        meta = storage.load_json(meta_path, None)
        if not meta:
            raise ValueError("upload_not_started")
        offset = int(offset)
        current_size = storage.file_size(tmp_path)
        if current_size is None:
            current_size = 0
        if current_size != offset:
            raise ValueError("upload_offset_mismatch")
        chunk = binascii.unhexlify(data_hex)
        with open(tmp_path, "ab") as f:
            f.write(chunk)
        written = offset + len(chunk)
        meta["written"] = written
        storage.save_json(meta_path, meta)
        return {"status": "ok", "message": "upload_chunk_written", "path": rel, "written": written}

    def upload_finish(self, relative_path):
        rel = self._safe_rel(relative_path)
        tmp_path = self._tmp_path(rel)
        meta_path = self._meta_path(rel)
        meta = storage.load_json(meta_path, None)
        if not meta:
            raise ValueError("upload_not_started")
        expected_size = int(meta.get("size", 0))
        actual_size = storage.file_size(tmp_path)
        if actual_size != expected_size:
            raise ValueError("upload_size_mismatch")
        actual_sha = storage.sha256_hex_file(tmp_path)
        expected_sha = meta.get("sha256", "")
        if expected_sha and actual_sha != expected_sha:
            storage.remove(tmp_path)
            storage.remove(meta_path)
            raise ValueError("upload_hash_mismatch")
        final_path = self._path(rel)
        storage.ensure_dir(storage.dirname(final_path))
        try:
            storage.remove(final_path)
            import os
            os.rename(tmp_path, final_path)
        finally:
            storage.remove(meta_path)
        return {
            "status": "ok",
            "message": "upload_complete",
            "path": rel,
            "size": actual_size,
            "sha256": actual_sha,
        }

    def download_begin(self, relative_path):
        rel = self._safe_rel(relative_path)
        path = self._path(rel)
        size = storage.file_size(path)
        if size is None:
            raise ValueError("file_not_found")
        return {
            "status": "ok",
            "message": "download_ready",
            "path": rel,
            "size": size,
            "sha256": storage.sha256_hex_file(path),
        }

    def download_chunk(self, relative_path, offset=0, length=1024):
        rel = self._safe_rel(relative_path)
        path = self._path(rel)
        offset = int(offset)
        length = max(1, int(length))
        data = storage.read_chunk(path, offset, length)
        next_offset = offset + len(data)
        size = storage.file_size(path)
        if size is None:
            raise ValueError("file_not_found")
        return {
            "status": "ok",
            "message": "download_chunk",
            "path": rel,
            "offset": offset,
            "next_offset": next_offset,
            "bytes": len(data),
            "data": binascii.hexlify(data).decode(),
            "has_more": next_offset < size,
        }
