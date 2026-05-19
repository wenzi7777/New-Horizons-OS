import storage
try:
    import ubinascii as binascii
except ImportError:
    import binascii


class FilesystemAPI:
    def __init__(self, root="data/files", tmp_root="data/tmp", scope_roots=None):
        self.root = root
        self.tmp_root = tmp_root
        self.scope_roots = {"user": root}
        if scope_roots:
            self.scope_roots.update(scope_roots)

    def _safe_scope(self, scope):
        name = str(scope or "user")
        if name not in self.scope_roots:
            raise ValueError("invalid_scope")
        return name

    def _safe_rel(self, relative_path):
        path = str(relative_path or "").replace("\\", "/").strip("/")
        parts = [part for part in path.split("/") if part]
        if not parts or any(part in (".", "..") for part in parts):
            raise ValueError("invalid_path")
        return "/".join(parts)

    def _root(self, scope="user"):
        return self.scope_roots[self._safe_scope(scope)]

    def _path(self, relative_path, scope="user"):
        return self._root(scope).rstrip("/") + "/" + self._safe_rel(relative_path)

    def _tmp_path(self, relative_path, scope="user"):
        return self.tmp_root.rstrip("/") + "/" + self._safe_scope(scope) + "/" + self._safe_rel(relative_path) + ".upload"

    def _meta_path(self, relative_path, scope="user"):
        return self.tmp_root.rstrip("/") + "/" + self._safe_scope(scope) + "/" + self._safe_rel(relative_path) + ".upload.json"

    def list_files(self, scope="user"):
        scope = self._safe_scope(scope)
        items = storage.list_tree(self._root(scope))
        for item in items:
            item["scope"] = scope
        return items

    def usage(self):
        usage = storage.fs_usage("/")
        scopes = {}
        scoped_bytes = 0
        for scope, root in self.scope_roots.items():
            size = storage.tree_size(root)
            scopes[scope] = size
            scoped_bytes += size
        tmp_bytes = storage.tree_size(self.tmp_root)
        known_bytes = scoped_bytes + tmp_bytes
        usage["scopes"] = scopes
        usage["tmp_bytes"] = tmp_bytes
        usage["known_bytes"] = known_bytes
        usage["other_bytes"] = max(0, int(usage.get("used_bytes", 0) or 0) - known_bytes)
        return usage

    def read_file(self, relative_path, scope="user"):
        path = self._path(relative_path, scope)
        text = storage.read_text(path)
        if text is not None:
            return {"scope": self._safe_scope(scope), "path": relative_path, "encoding": "text", "content": text}
        data = storage.read_bytes(path)
        if data is None:
            return None
        return {"scope": self._safe_scope(scope), "path": relative_path, "encoding": "hex", "content": data.hex()}

    def delete_file(self, relative_path, scope="user"):
        return storage.remove(self._path(relative_path, scope))

    def upload_begin(self, relative_path, size, sha256="", scope="user"):
        scope = self._safe_scope(scope)
        rel = self._safe_rel(relative_path)
        tmp_path = self._tmp_path(rel, scope)
        meta_path = self._meta_path(rel, scope)
        storage.ensure_dir(storage.dirname(tmp_path))
        storage.ensure_dir(storage.dirname(meta_path))
        storage.remove(tmp_path)
        storage.save_json(meta_path, {
            "scope": scope,
            "path": rel,
            "size": int(size),
            "sha256": str(sha256 or ""),
            "written": 0,
        })
        with open(tmp_path, "wb"):
            pass
        return {"status": "ok", "message": "upload_started", "scope": scope, "path": rel, "size": int(size)}

    def upload_chunk(self, relative_path, offset, data_hex, scope="user"):
        scope = self._safe_scope(scope)
        rel = self._safe_rel(relative_path)
        tmp_path = self._tmp_path(rel, scope)
        meta_path = self._meta_path(rel, scope)
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
        return {"status": "ok", "message": "upload_chunk_written", "scope": scope, "path": rel, "written": written}

    def upload_finish(self, relative_path, scope="user"):
        scope = self._safe_scope(scope)
        rel = self._safe_rel(relative_path)
        tmp_path = self._tmp_path(rel, scope)
        meta_path = self._meta_path(rel, scope)
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
        final_path = self._path(rel, scope)
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
            "scope": scope,
            "path": rel,
            "size": actual_size,
            "sha256": actual_sha,
        }

    def download_begin(self, relative_path, scope="user"):
        scope = self._safe_scope(scope)
        rel = self._safe_rel(relative_path)
        path = self._path(rel, scope)
        size = storage.file_size(path)
        if size is None:
            raise ValueError("file_not_found")
        return {
            "status": "ok",
            "message": "download_ready",
            "scope": scope,
            "path": rel,
            "size": size,
            "sha256": storage.sha256_hex_file(path),
        }

    def download_chunk(self, relative_path, offset=0, length=1024, scope="user"):
        scope = self._safe_scope(scope)
        rel = self._safe_rel(relative_path)
        path = self._path(rel, scope)
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
            "scope": scope,
            "path": rel,
            "offset": offset,
            "next_offset": next_offset,
            "bytes": len(data),
            "data": binascii.hexlify(data).decode(),
            "has_more": next_offset < size,
        }
