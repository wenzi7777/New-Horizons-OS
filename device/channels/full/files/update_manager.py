import json

import storage
from manifest_update import ManifestPlanner

try:
    import urequests as requests
except ImportError:  # pragma: no cover - host fallback
    requests = None
    try:
        import urllib.request
    except ImportError:
        urllib = None


class UpdateManager:
    def __init__(self, config_store, logger=None, root_dir="."):
        self.config_store = config_store
        self.logger = logger
        self.root_dir = root_dir
        self.planner = ManifestPlanner(root_dir)
        self.cached_manifest = None
        self.cached_plan = None

    def check(self):
        runtime = self.config_store.load_runtime()
        update_cfg = runtime.get("update", {})
        manifest_url = update_cfg.get("manifest_url", "")
        if not update_cfg.get("enabled") or not manifest_url:
            return {
                "status": "disabled",
                "message": "update_disabled",
                "downloads": [],
                "reboot_required": False,
            }

        manifest = self._fetch_json(manifest_url)
        self.cached_manifest = manifest
        self.cached_plan = self.planner.plan(manifest)
        state = self.config_store.load_update_state()
        state["last_result"] = "checked"
        self.config_store.save_update_state(state)
        return {
            "status": "ok",
            "message": "manifest_checked",
            "downloads": self.cached_plan["downloads"],
            "reboot_required": self.cached_plan["reboot_required"],
        }

    def apply(self):
        if self.cached_manifest is None or self.cached_plan is None:
            result = self.check()
            if result["status"] != "ok":
                return result

        applied = []
        for item in self.cached_plan["downloads"]:
            payload = self._fetch_bytes(item["url"])
            if not self.planner.verify_download(item["sha256"], payload):
                return {
                    "status": "error",
                    "message": "hash_mismatch",
                    "error": item["path"],
                    "reboot_required": False,
                }
            self.planner.apply_file(item["path"], payload)
            applied.append(item["path"])

        state = self.config_store.load_update_state()
        state["last_result"] = "applied"
        state["reboot_required"] = bool(self.cached_plan["reboot_required"])
        self.config_store.save_update_state(state)
        return {
            "status": "ok",
            "message": "update_applied",
            "applied": applied,
            "reboot_required": state["reboot_required"],
        }

    def _fetch_json(self, url):
        payload = self._fetch_bytes(url)
        return json.loads(payload.decode())

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
