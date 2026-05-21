# wifi_manager.py
import time
import gc
import network

import config
import secrets
from device_identity import get_device_suffix, get_device_uid
import gateway_discovery
import storage


WiFiSetupPortal = None


class WiFiManager:
    def __init__(self, config_store=None, logger=None):
        self.sta = None
        self.ap = None
        self.config_store = config_store
        self.logger = logger
        self.state = "idle"
        self.last_error = ""
        self.last_setup_result = ""
        self.portal = None

    def connect(self):
        wifi_mode = config.WIFI_MODE
        if self.config_store is not None:
            wifi_mode = self.config_store.load_network().get("wifi_mode", config.WIFI_MODE)

        if wifi_mode == "AP":
            return self.start_setup_portal("config_ap_mode")
        return self.connect_sta()

    def connect_sta(self, ssid=None, password=None, keep_setup_portal=False):
        gc.collect()
        self.state = "normal_boot"
        self.last_error = ""
        if not self.setup_active():
            self._disable_ap()

        sta = self._ensure_sta()
        sta.active(True)
        time.sleep_ms(300)

        if sta.isconnected():
            self._log_info("wifi_sta_already_connected ip={}".format(sta.ifconfig()[0]))
            self.state = "wifi_connected"
            return True

        if ssid is None:
            ssid, password = self._credentials()

        if not ssid:
            self.last_error = "missing_credentials"
            self.state = "wifi_failed"
            return False

        self._log_info("wifi_sta_connect_start ssid={}".format(ssid))
        for attempt in range(1, 4):
            self._log_info("wifi_sta_connect_attempt={} free={}".format(attempt, gc.mem_free()))

            try:
                try:
                    sta.disconnect()
                    time.sleep_ms(300)
                except Exception:
                    pass

                sta.connect(ssid, password)

                for _ in range(20):
                    if sta.isconnected():
                        self._log_info("wifi_sta_connected ip={}".format(sta.ifconfig()[0]))
                        if not keep_setup_portal:
                            self.stop_setup_portal()
                            self._disable_ap()
                        self.state = "wifi_connected"
                        return True

                    time.sleep(1)

                self._log_warn("wifi_sta_connect_timeout attempt={}".format(attempt))

            except RuntimeError as e:
                self._log_warn("wifi_sta_connect_runtime_error {} free={}".format(e, gc.mem_free()))

            except OSError as e:
                self._log_warn("wifi_sta_connect_os_error {} free={}".format(e, gc.mem_free()))

            self._reset_sta_interface()
            gc.collect()

        self._log_warn("wifi_sta_connect_failed")
        self.last_error = "wifi_connect_failed"
        self.state = "wifi_failed"
        return False

    def _reset_sta_interface(self):
        self._log_info("wifi_sta_reset")
        sta = self._ensure_sta()

        try:
            sta.active(False)
            time.sleep_ms(800)
        except Exception as e:
            self._log_warn("wifi_sta_active_false_warning {}".format(e))

        try:
            sta.active(True)
            time.sleep_ms(800)
        except Exception as e:
            self._log_warn("wifi_sta_active_true_warning {}".format(e))

    def start_setup_portal(self, reason="manual"):
        gc.collect()
        self.state = "wifi_setup_active"
        self.last_setup_result = reason

        ap = self._ensure_ap()
        ap_ssid = self.ap_ssid()

        try:
            ap.active(False)
        except Exception:
            pass

        try:
            ap.active(True)
            time.sleep_ms(300)
        except Exception:
            pass

        ap.config(essid=ap_ssid, password=config.SETUP_AP_PASSWORD)
        gc.collect()

        portal = self._ensure_portal()
        portal.start()
        ifconfig = ap.ifconfig()
        self._log_info("wifi_setup_ap_started ssid={} ip={}".format(ap_ssid, ifconfig[0]))
        return True

    def service_setup_portal(self):
        if self.portal is None:
            return False
        return self.portal.service()

    def stop_setup_portal(self):
        if self.portal is not None:
            self.portal.stop()
        self._disable_ap()

    def setup_active(self):
        return bool(self.portal is not None and self.portal.active)

    def scan_networks(self):
        sta = self._ensure_sta()
        was_active = False
        try:
            was_active = sta.active()
        except Exception:
            was_active = False
        if not was_active:
            try:
                sta.active(True)
                time.sleep_ms(200)
            except Exception:
                pass

        results = []
        try:
            for item in sta.scan():
                ssid_raw = item[0]
                ssid = ssid_raw.decode() if isinstance(ssid_raw, bytes) else str(ssid_raw)
                if not ssid:
                    continue
                results.append({
                    "ssid": ssid,
                    "rssi": int(item[3]),
                    "security": self._auth_name(int(item[4])),
                })
        except Exception as exc:
            self.last_error = "scan_failed"
            if self.logger:
                self.logger.warn("wifi_scan_failed {}".format(exc))
        finally:
            if not was_active and not self.is_connected():
                try:
                    sta.active(False)
                except Exception:
                    pass

        results.sort(key=lambda item: item["rssi"], reverse=True)
        seen = {}
        ordered = []
        for item in results:
            if item["ssid"] in seen:
                continue
            seen[item["ssid"]] = True
            ordered.append(item)
        return ordered[:12]

    def apply_credentials(
        self,
        ssid,
        password,
        release_url="",
        log_enabled="",
        log_capacity="",
    ):
        ssid = (ssid or "").strip()
        password = password or ""
        if not ssid:
            self.last_error = "missing_ssid"
            self.last_setup_result = "missing_ssid"
            return {"ok": False, "message": "SSID is required"}

        if self.config_store is not None:
            self.config_store.update_network({
                "wifi_mode": "STA",
                "ssid": ssid,
                "password": password,
                "setup_method": "softap_webui",
                "last_ssid": ssid,
            })
            runtime_patch = self._runtime_patch_for_gateway_reset()
            runtime_patch.update(self._runtime_patch_for_logging(log_enabled, log_capacity))
            runtime_patch.update(self._runtime_patch_for_github_update())
            if runtime_patch:
                self.config_store.update_runtime(runtime_patch)

        ok = self.connect_sta(ssid, password, keep_setup_portal=True)
        if ok and self.discover_gateway(reason="wifi_setup").get("ok"):
            self.last_setup_result = "connected_gateway_discovered"
            self.stop_setup_portal()
            return {
                "ok": True,
                "message": "Connected to {} and discovered gateway".format(ssid),
                "ifconfig": self._ensure_sta().ifconfig(),
            }

        if ok:
            self.last_setup_result = "gateway_discovery_failed"
            return {
                "ok": False,
                "wifi_connected": True,
                "message": "Wi-Fi connected, but no New Horizons Gateway was discovered.",
                "ifconfig": self._ensure_sta().ifconfig(),
            }

        self.last_setup_result = "connect_failed"
        return {"ok": False, "message": "Failed to connect to {}".format(ssid)}

    def clear_credentials(self):
        if self.config_store is not None:
            network_cfg = self.config_store.load_network()
            network_cfg.update({
                "wifi_mode": "STA",
                "ssid": "",
                "password": "",
                "last_ssid": "",
            })
            self.config_store.save_network(network_cfg)
        self.last_setup_result = "credentials_cleared"
        self.last_error = ""

    def is_connected(self):
        if self.sta is None:
            return False
        return self.sta.isconnected()

    def portal_status(self):
        network_cfg = self.config_store.load_network() if self.config_store is not None else {}
        runtime_cfg = self.config_store.load_runtime() if self.config_store is not None else {}
        portal_ip = config.SETUP_PORTAL_HOST
        portal_domain = getattr(config, "SETUP_PORTAL_DOMAIN", "")
        ap_ssid = self.ap_ssid()
        if self.ap is not None:
            try:
                portal_ip = self.ap.ifconfig()[0]
            except Exception:
                portal_ip = config.SETUP_PORTAL_HOST
        os_installed = storage.exists(getattr(config, "OS_DIR", "nhos") + "/app.mpy")
        return {
            "active": self.setup_active(),
            "state": self.state,
            "mode": runtime_cfg.get("mode", "normal"),
            "os_installed": os_installed,
            "release_url": self._github_release_url() or runtime_cfg.get("update", {}).get("release_url", ""),
            "ap_ssid": ap_ssid,
            "portal_ip": portal_ip,
            "portal_domain": portal_domain,
            "portal_url": self._portal_url(portal_domain or portal_ip),
            "portal_ip_url": self._portal_url(portal_ip),
            "saved_ssid": network_cfg.get("ssid", ""),
            "last_error": self.last_error,
            "last_setup_result": self.last_setup_result,
            "server": dict(runtime_cfg.get("server", {})),
            "gateway_discovery": dict(runtime_cfg.get("gateway_discovery", {})),
            "transport": dict(runtime_cfg.get("transport", {})),
            "logging": dict(runtime_cfg.get("logging", {})),
        }

    def discover_gateway(self, reason="manual"):
        if not self.is_connected():
            result = {"ok": False, "error": "wifi_not_connected"}
            self._save_gateway_result(result, reason)
            self.last_error = "gateway_discovery_failed"
            return result
        try:
            result = gateway_discovery.discover_gateway(get_device_uid(), self._runtime_mode())
        except Exception as exc:
            result = {"ok": False, "error": str(exc)}
        self._save_gateway_result(result, reason)
        if result.get("ok"):
            self.last_error = ""
            self._log_info(
                "gateway_discovery_ok host={} tcp={} udp={} id={}".format(
                    result.get("host", ""),
                    result.get("tcp_port", ""),
                    result.get("udp_port", ""),
                    result.get("gateway_id", ""),
                )
            )
        else:
            self.last_error = "gateway_discovery_failed"
            self._log_warn("gateway_discovery_failed error={}".format(result.get("error", "unknown")))
        return result

    def _runtime_mode(self):
        if self.config_store is None:
            return "normal"
        return self.config_store.load_runtime().get("mode", "normal")

    def _runtime_patch_for_gateway_reset(self):
        return {
            "server": {
                "host": "",
                "tcp_port": int(getattr(config, "DEFAULT_TCP_CONTROL_PORT", 22345)),
                "udp_port": int(getattr(config, "DEFAULT_UDP_STREAM_PORT", 13250)),
                "source": "discovery",
                "gateway_id": "",
            },
            "transport": {"mode": "udp_tcp"},
            "gateway_discovery": {
                "enabled": True,
                "port": int(getattr(config, "DEFAULT_GATEWAY_DISCOVERY_PORT", 22346)),
                "gateway_id": "",
                "host": "",
                "tcp_port": int(getattr(config, "DEFAULT_TCP_CONTROL_PORT", 22345)),
                "udp_port": int(getattr(config, "DEFAULT_UDP_STREAM_PORT", 13250)),
                "last_success_ms": 0,
                "last_error": "",
                "source": "discovery",
                "reason": "reset",
            },
        }

    def _save_gateway_result(self, result, reason):
        if self.config_store is None:
            return
        runtime = self.config_store.load_runtime()
        current_server = dict(runtime.get("server", {}))
        discovery = dict(runtime.get("gateway_discovery", {}))
        discovery.update({
            "enabled": True,
            "port": int(getattr(config, "DEFAULT_GATEWAY_DISCOVERY_PORT", 22346)),
            "source": "discovery",
            "reason": reason,
        })
        if result.get("ok"):
            server = {
                "host": result.get("host", ""),
                "tcp_port": int(result.get("tcp_port") or getattr(config, "DEFAULT_TCP_CONTROL_PORT", 22345)),
                "udp_port": int(result.get("udp_port") or getattr(config, "DEFAULT_UDP_STREAM_PORT", 13250)),
                "source": "discovery",
                "gateway_id": result.get("gateway_id", ""),
            }
            discovery.update({
                "gateway_id": server["gateway_id"],
                "host": server["host"],
                "tcp_port": server["tcp_port"],
                "udp_port": server["udp_port"],
                "last_success_ms": int(result.get("discovered_at_ms") or 0),
                "last_error": "",
                "priority": int(result.get("priority") or 0),
            })
            self.config_store.update_runtime({
                "server": server,
                "gateway_discovery": discovery,
                "transport": {"mode": "udp_tcp"},
            })
            return
        discovery["last_error"] = str(result.get("error") or "no_gateway")
        self.config_store.update_runtime({
            "server": current_server,
            "gateway_discovery": discovery,
            "transport": {"mode": "udp_tcp"},
        })

    def _runtime_patch_for_github_update(self):
        release_url = self._github_release_url()
        if not release_url:
            return {}
        current_runtime = self.config_store.load_runtime() if self.config_store is not None else {}
        current_update = dict(current_runtime.get("update", {}))
        current_update["release_url"] = release_url
        current_update["source"] = "github"
        sources = current_update.get("sources", {})
        if isinstance(sources, dict) and isinstance(sources.get("github"), dict):
            current_update["sources"] = {"github": dict(sources.get("github", {}))}
        return {"update": current_update}

    def _github_release_url(self):
        return str(
            getattr(config, "DEFAULT_RELEASE_URL", "")
            or getattr(config, "GITHUB_RELEASE_URL", "")
            or ""
        ).strip()

    def _runtime_patch_for_logging(self, log_enabled="", log_capacity=""):
        patch = {}
        current_runtime = self.config_store.load_runtime() if self.config_store is not None else {}
        current_logging = dict(current_runtime.get("logging", {}))
        normalized_enabled = self._normalize_bool(log_enabled)
        normalized_capacity = str(log_capacity or "").strip().lower()
        if normalized_capacity not in ("default", "extended"):
            normalized_capacity = ""
        if normalized_enabled is not None or normalized_capacity:
            patch["logging"] = {
                "enabled": current_logging.get("enabled", True) if normalized_enabled is None else normalized_enabled,
                "capacity": normalized_capacity or current_logging.get("capacity", "default"),
                "serial": "status",
            }
        return patch

    def _normalize_bool(self, value):
        normalized = str(value or "").strip().lower()
        if normalized in ("1", "true", "yes", "on"):
            return True
        if normalized in ("0", "false", "no", "off"):
            return False
        return None

    def _credentials(self):
        ssid = secrets.WIFI_SSID
        password = secrets.WIFI_PASSWORD

        if self.config_store is not None:
            network_cfg = self.config_store.load_network()
            if network_cfg.get("ssid"):
                ssid = network_cfg.get("ssid")
                password = network_cfg.get("password", "")

        return ssid, password

    def _ensure_sta(self):
        if self.sta is None:
            self.sta = network.WLAN(network.STA_IF)
        return self.sta

    def _ensure_ap(self):
        if self.ap is None:
            self.ap = network.WLAN(network.AP_IF)
        return self.ap

    def _ensure_portal(self):
        global WiFiSetupPortal
        if self.portal is None:
            if WiFiSetupPortal is None:
                from wifi_portal import WiFiSetupPortal as LoadedWiFiSetupPortal
                WiFiSetupPortal = LoadedWiFiSetupPortal
            self.portal = WiFiSetupPortal(self, config, self.logger)
        return self.portal

    def _disable_ap(self):
        # Normal STA boots should not allocate AP_IF just to disable an AP that
        # was never created; on memory-tight boards that allocation can fail.
        if self.ap is None:
            return
        ap = self.ap
        try:
            ap.active(False)
        except Exception:
            pass

    def ap_ssid(self):
        suffix = get_device_suffix()
        if suffix:
            return "{}-{}".format(config.SETUP_AP_SSID_PREFIX, suffix)
        return config.SETUP_AP_SSID_PREFIX

    def _mac_suffix(self):
        return get_device_suffix()

    def _portal_url(self, host):
        if int(config.SETUP_PORTAL_PORT) == 80:
            return "http://{}".format(host)
        return "http://{}:{}".format(host, config.SETUP_PORTAL_PORT)

    def _log_info(self, message):
        if self.logger:
            self.logger.info(message)
        else:
            print(message)

    def _log_warn(self, message):
        if self.logger:
            self.logger.warn(message)
        else:
            print(message)

    def _auth_name(self, value):
        mapping = {
            0: "open",
            1: "wep",
            2: "wpa-psk",
            3: "wpa2-psk",
            4: "wpa/wpa2-psk",
            5: "wpa2-enterprise",
            6: "wpa3-psk",
            7: "wpa2/wpa3-psk",
        }
        return mapping.get(value, "unknown")
