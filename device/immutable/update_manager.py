import json
import os
import time

import storage
from manifest_update import ManifestPlanner

try:
    import urequests as requests
except ImportError:
    requests = None
    try:
        import urllib.request
    except ImportError:
        urllib = None


def now_ms():
    if hasattr(time, "ticks_ms"):
        return int(time.ticks_ms())
    return int(time.time() * 1000)


class UpdateManager:
    DOWNLOAD_CHUNK_SIZE = 1024

    def __init__(self, config_store, logger=None, root_dir="."):
        self.config_store = config_store
        self.logger = logger
        self.root_dir = root_dir
        self.planner = ManifestPlanner(root_dir)
        self.cached_manifest = None
        self.cached_plan = None
        self.active_job = None

    def status(self):
        return self.config_store.load_update_state()

    def is_busy(self):
        return self.active_job is not None

    def start_check(self):
        return self._start_job("check")

    def start_apply(self):
        return self._start_job("apply")

    def check(self):
        start = self.start_check()
        if start["status"] != "ok":
            return start
        result = None
        while self.active_job is not None:
            result = self.service()
        return result or start

    def apply(self):
        start = self.start_apply()
        if start["status"] != "ok":
            return start
        result = None
        while self.active_job is not None:
            result = self.service()
        return result or start

    def service(self):
        if self.active_job is None:
            return None

        job = self.active_job
        if job["phase"] == "checking_manifest":
            return self._service_manifest(job)
        if job["phase"] == "downloading":
            return self._service_download(job)
        return None

    def _start_job(self, operation):
        if self.active_job is not None:
            state = self.status()
            return {
                "status": "error",
                "message": "update_busy",
                "error": state.get("phase", "busy"),
                "reboot_required": state.get("reboot_required", False),
                "update_state": state,
            }

        self.cached_manifest = None
        self.cached_plan = None
        self.active_job = {
            "operation": operation,
            "phase": "checking_manifest",
            "downloads": [],
            "index": 0,
            "applied": [],
        }
        state = self._save_state({
            "phase": "checking_manifest",
            "operation": operation,
            "total_files": 0,
            "applied_files": 0,
            "current_file": "",
            "last_error": "",
            "last_result": "",
            "reboot_required": False,
            "started_at_ms": now_ms(),
            "finished_at_ms": 0,
        })
        return {
            "status": "ok",
            "message": "update_started",
            "operation": operation,
            "reboot_required": False,
            "update_state": state,
        }

    def _service_manifest(self, job):
        runtime = self.config_store.load_runtime()
        update_cfg = runtime.get("update", {})
        manifest_url = update_cfg.get("manifest_url", "")
        if not update_cfg.get("enabled") or not manifest_url:
            return self._finish_disabled()

        try:
            manifest = self._fetch_json(manifest_url)
        except Exception as exc:
            return self._finish_error("manifest_fetch_failed", str(exc))

        try:
            self.cached_manifest = manifest
            self.cached_plan = self.planner.plan(manifest)
            downloads = self.cached_plan.get("downloads", [])
        except Exception as exc:
            self.cached_manifest = None
            self.cached_plan = None
            return self._finish_error("manifest_process_failed", str(exc))
        reboot_required = bool(self.cached_plan.get("reboot_required"))
        checked_at = now_ms()

        state_patch = {
            "last_check_ms": checked_at,
            "last_error": "",
            "reboot_required": reboot_required,
            "total_files": len(downloads),
            "applied_files": 0,
            "current_file": "",
        }

        if job["operation"] == "check":
            return self._finish_success(
                "manifest_checked",
                [],
                reboot_required,
                extra_patch=state_patch,
                phase="ready",
                last_result="checked",
                downloads=downloads,
            )

        job["downloads"] = downloads
        job["phase"] = "downloading"
        state = self._save_state(dict(state_patch, phase="downloading"))
        if not downloads:
            return self._finish_success(
                "update_applied",
                [],
                reboot_required,
                extra_patch=state_patch,
            )
        return {
            "status": "ok",
            "message": "update_progress",
            "reboot_required": reboot_required,
            "update_state": state,
        }

    def _service_download(self, job):
        downloads = job["downloads"]
        item = downloads[job["index"]]
        state = self._save_state({
            "phase": "downloading",
            "current_file": item["path"],
            "applied_files": len(job["applied"]),
            "total_files": len(downloads),
            "last_error": "",
        })
        try:
            self._download_to_path(item["url"], self.planner._local_path(item["path"]), item["sha256"])
        except Exception as exc:
            return self._finish_error("download_failed", "{}: {}".format(item["path"], exc))

        job["applied"].append(item["path"])
        job["index"] += 1
        reboot_required = bool(self.cached_plan.get("reboot_required"))

        if job["index"] >= len(downloads):
            return self._finish_success(
                "update_applied",
                job["applied"],
                reboot_required,
            )

        next_item = downloads[job["index"]]
        next_state = self._save_state({
            "phase": "downloading",
            "current_file": next_item["path"],
            "applied_files": len(job["applied"]),
            "total_files": len(downloads),
            "reboot_required": reboot_required,
        })
        return {
            "status": "ok",
            "message": "update_progress",
            "applied": list(job["applied"]),
            "reboot_required": reboot_required,
            "update_state": next_state,
        }

    def _finish_success(self, message, applied, reboot_required, extra_patch=None, phase="done", last_result="applied", downloads=None):
        state_patch = {
            "phase": phase,
            "applied_files": len(applied),
            "current_file": "",
            "last_error": "",
            "last_result": last_result,
            "reboot_required": bool(reboot_required),
            "finished_at_ms": now_ms(),
        }
        if extra_patch:
            state_patch.update(extra_patch)
        state = self._save_state(state_patch)
        self.active_job = None
        response = {
            "status": "ok",
            "message": message,
            "reboot_required": bool(reboot_required),
            "update_state": state,
        }
        if applied:
            response["applied"] = list(applied)
        if downloads is not None:
            response["downloads"] = downloads
        return response

    def _finish_disabled(self):
        state = self._save_state({
            "phase": "idle",
            "operation": "",
            "total_files": 0,
            "applied_files": 0,
            "current_file": "",
            "last_error": "",
            "last_result": "disabled",
            "reboot_required": False,
            "finished_at_ms": now_ms(),
        })
        self.active_job = None
        return {
            "status": "disabled",
            "message": "update_disabled",
            "downloads": [],
            "reboot_required": False,
            "update_state": state,
        }

    def _finish_error(self, message, error):
        state = self._save_state({
            "phase": "error",
            "last_result": "error",
            "last_error": str(error),
            "finished_at_ms": now_ms(),
        })
        self.active_job = None
        return {
            "status": "error",
            "message": message,
            "error": str(error),
            "reboot_required": False,
            "update_state": state,
        }

    def _save_state(self, patch):
        state = self.config_store.load_update_state()
        state.update(patch)
        self.config_store.save_update_state(state)
        return state

    def _fetch_json(self, url):
        return json.loads(self._fetch_bytes(url).decode())

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

    def _download_to_path(self, url, local_path, expected_sha256):
        tmp_path = local_path + ".ota_tmp"
        storage.ensure_dir(storage.dirname(local_path))
        hasher = self._new_hasher()
        stream = None
        closer = None

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

        actual_sha256 = self._hexdigest(hasher)
        if actual_sha256 != expected_sha256:
            storage.remove(tmp_path)
            raise ValueError("hash_mismatch")

        try:
            storage.remove(local_path)
            os.rename(tmp_path, local_path)
        except Exception:
            storage.remove(tmp_path)
            raise

    def _new_hasher(self):
        if storage.hashlib is None:
            raise RuntimeError("hashlib unavailable")
        return storage.hashlib.sha256()

    def _hexdigest(self, hasher):
        if hasattr(hasher, "hexdigest"):
            return hasher.hexdigest()
        return storage.binascii.hexlify(hasher.digest()).decode()
