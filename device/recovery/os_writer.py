try:
    import uos as os
except ImportError:  # pragma: no cover - CPython fallback
    import os

import storage

try:
    import gc
except ImportError:  # pragma: no cover - CPython fallback
    gc = None

try:
    import urequests as requests
except ImportError:
    requests = None
    try:
        import urllib.request
    except ImportError:
        urllib = None


class OSWriter:
    DOWNLOAD_CHUNK_SIZE = 1024
    OS_TARGET_ROOT = "nhos"

    def __init__(self, root_dir=".", logger=None, progress=None):
        self.root_dir = root_dir.rstrip("/") if root_dir not in ("", ".") else "."
        self.logger = logger
        self.progress = progress
        self.state_path = self._rooted("device_state/os_state.tlv")

    def check_os_release(self, release_url):
        release = self._fetch_tlv(release_url)
        manifest_url = release.get("manifest_url", "")
        if not manifest_url:
            raise ValueError("missing_manifest_url")
        return {
            "status": "ok",
            "message": "os_release_checked",
            "latest_version": release.get("latest", release.get("version", "")),
            "manifest_url": manifest_url,
            "release": release,
        }

    def write_os(self, release_url):
        release = self._fetch_tlv(release_url)
        manifest_url = release.get("manifest_url", "")
        if not manifest_url:
            raise ValueError("missing_manifest_url")
        manifest = self._fetch_tlv(manifest_url)
        version = manifest.get("version", release.get("latest", ""))
        summary = self._summarize(manifest)
        self._emit("planned", version, summary, "")

        downloaded = 0
        for item in self._iter_downloads(manifest):
            self._collect()
            staged_path = self._stage_path(item["path"])
            self._emit("downloading", version, summary, item["path"], downloaded=downloaded)
            self._download_to_path(item["url"], staged_path, item["sha256"])
            src = self._stage_path(item["path"])
            dst = self._os_path(item["path"], manifest)
            storage.ensure_dir(storage.dirname(dst))
            storage.remove(dst)
            os.rename(src, dst)
            downloaded += 1
            self._emit("file_done", version, summary, item["path"], downloaded=downloaded)
            self._collect()

        deleted = 0
        for rel in self._iter_deletes(manifest):
            if storage.remove(self._os_path(rel, manifest)):
                deleted += 1

        state = {
            "version": version,
            "manifest_url": manifest_url,
            "downloaded_files": downloaded,
            "skipped_files": summary["skipped_files"],
            "deleted_files": deleted,
            "last_result": "applied",
        }
        storage.save_tlv(self.state_path, state)
        self._emit("complete", version, summary, "", downloaded=downloaded)
        return {
            "status": "ok",
            "message": "os_write_complete",
            "version": version,
            "downloaded_files": downloaded,
            "skipped_files": summary["skipped_files"],
            "deleted_files": deleted,
            "reboot_required": True,
        }

    def _summarize(self, manifest):
        total = 0
        downloads = 0
        skips = 0
        for entry in manifest.get("files", []):
            total += 1
            rel = self._safe_rel(entry.get("path", ""))
            expected = entry.get("sha256", "")
            current = storage.sha256_hex_file(self._os_path(rel, manifest))
            if current == expected:
                skips += 1
                continue
            downloads += 1
        return {
            "total_files": total,
            "download_files": downloads,
            "skipped_files": skips,
        }

    def _iter_downloads(self, manifest):
        base_url = manifest.get("base_url", "")
        for entry in manifest.get("files", []):
            rel = self._safe_rel(entry.get("path", ""))
            expected = entry.get("sha256", "")
            current = storage.sha256_hex_file(self._os_path(rel, manifest))
            if current == expected:
                continue
            yield {
                "path": rel,
                "url": self._make_url(base_url, rel),
                "sha256": expected,
                "size": int(entry.get("size", 0)),
            }

    def _iter_deletes(self, manifest):
        for path in manifest.get("delete", []):
            if path:
                yield self._safe_rel(path)

    def _download_to_path(self, url, local_path, expected_sha256):
        storage.ensure_dir(storage.dirname(local_path))
        tmp_path = local_path + ".tmp"
        hasher = self._new_hasher()
        stream = None
        closer = None
        chunk = None
        self._collect()
        try:
            if requests is not None:
                resp = requests.get(url)
                stream = getattr(resp, "raw", resp)
                closer = resp
            else:
                req = urllib.request.urlopen(url)  # type: ignore[name-defined]
                stream = req
                closer = req
            with open(tmp_path, "wb") as handle:
                while True:
                    chunk = stream.read(self.DOWNLOAD_CHUNK_SIZE)
                    if not chunk:
                        break
                    hasher.update(chunk)
                    handle.write(chunk)
        finally:
            if closer is not None:
                try:
                    closer.close()
                except Exception:
                    pass
            stream = None
            closer = None
            chunk = None
            self._collect()
        actual = self._hexdigest(hasher)
        if actual != expected_sha256:
            storage.remove(tmp_path)
            raise ValueError("hash_mismatch")
        storage.remove(local_path)
        os.rename(tmp_path, local_path)

    def _fetch_tlv(self, url):
        payload = self._fetch_bytes(url)
        return storage.loads_tlv(payload)

    def _fetch_bytes(self, url):
        if requests is not None:
            resp = requests.get(url)
            try:
                return resp.content
            finally:
                try:
                    resp.close()
                except Exception:
                    pass
        req = urllib.request.urlopen(url)  # type: ignore[name-defined]
        try:
            return req.read()
        finally:
            req.close()

    def _rooted(self, relative_path):
        rel = relative_path.strip("/")
        return rel if self.root_dir in ("", ".") else self.root_dir + "/" + rel

    def _target_root(self, manifest):
        target = manifest.get("target_root", "/" + self.OS_TARGET_ROOT)
        target = str(target or "/" + self.OS_TARGET_ROOT).strip("/")
        if not target:
            target = self.OS_TARGET_ROOT
        if target != self.OS_TARGET_ROOT:
            raise ValueError("invalid_target_root")
        return target

    def _os_path(self, relative_path, manifest=None):
        target_root = self._target_root(manifest or {"target_root": "/" + self.OS_TARGET_ROOT})
        return self._rooted(target_root + "/" + self._safe_rel(relative_path))

    def _stage_path(self, relative_path):
        return self._rooted("ota_stage/" + self.OS_TARGET_ROOT + "/" + self._safe_rel(relative_path))

    def _safe_rel(self, relative_path):
        path = str(relative_path or "").replace("\\", "/").strip("/")
        parts = [part for part in path.split("/") if part]
        if not parts or any(part in (".", "..") for part in parts):
            raise ValueError("invalid_path")
        return "/".join(parts)

    def _make_url(self, base_url, path):
        return path if not base_url else base_url.rstrip("/") + "/" + path.lstrip("/")

    def _new_hasher(self):
        if storage.hashlib is None:
            raise RuntimeError("hashlib unavailable")
        return storage.hashlib.sha256()

    def _hexdigest(self, hasher):
        if hasattr(hasher, "hexdigest"):
            return hasher.hexdigest()
        return storage.binascii.hexlify(hasher.digest()).decode()

    def _collect(self):
        if gc is not None:
            try:
                gc.collect()
            except Exception:
                pass

    def _emit(self, phase, version, summary, current_file, downloaded=0):
        payload = {
            "message": "os_write_progress" if phase != "complete" else "os_write_complete",
            "phase": phase,
            "version": version,
            "total_files": int(summary.get("total_files", 0)),
            "download_files": int(summary.get("download_files", 0)),
            "skipped_files": int(summary.get("skipped_files", 0)),
            "written_files": downloaded,
            "current_file": current_file,
        }
        if self.progress:
            self.progress(payload)
        if self.logger:
            try:
                self.logger.info("{} {}".format(payload["message"], phase))
            except Exception:
                pass
