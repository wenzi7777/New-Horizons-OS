# wifi_manager.py
import time
import gc
import network

import config
import secrets
from device_identity import get_device_suffix
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

    def connect_sta(self, ssid=None, password=None):
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
        server_profile=None,
        master_host="",
        master_port="",
        data_host="",
        data_port="",
        mqtt_host="",
        mqtt_port="",
        mqtt_tls="",
        transport_mode="",
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
            runtime_patch = self._runtime_patch_for_server_profile(
                server_profile,
                master_host,
                master_port,
                data_host,
                data_port,
            )
            runtime_patch = runtime_patch or {}
            selected_profile = runtime_patch.get("server_profile", "") or str(server_profile or "").strip()
            if not selected_profile:
                selected_profile = self._selected_server_profile(self.config_store.load_runtime())
            runtime_patch.update(
                self._runtime_patch_for_transport(
                    selected_profile,
                    mqtt_host,
                    mqtt_port,
                )
            )
            runtime_patch.update(self._runtime_patch_for_logging(log_enabled, log_capacity))
            runtime_patch.update(self._runtime_patch_for_github_update())
            if runtime_patch:
                self.config_store.update_runtime(runtime_patch)

        ok = self.connect_sta(ssid, password)
        if ok:
            self.last_setup_result = "connected"
            self.stop_setup_portal()
            return {
                "ok": True,
                "message": "Connected to {}".format(ssid),
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

    def _version_status(self, runtime_cfg, os_installed):
        system = runtime_cfg.get("system", {}) if isinstance(runtime_cfg, dict) else {}
        if not isinstance(system, dict):
            system = {}
        runtime_version = getattr(config, "RUNTIME_VERSION", "unknown")
        recovery_version = system.get("recovery_version", "") or getattr(
            config,
            "RECOVERY_VERSION",
            getattr(config, "RECOVERY_FIRMWARE_VERSION", getattr(config, "FIRMWARE_VERSION", "unknown")),
        )
        os_version = system.get("os_version", "")
        if not os_version and self.config_store is not None:
            try:
                update_state = self.config_store.load_update_state()
                if update_state.get("operation") in ("check_os_release", "write_os"):
                    os_version = update_state.get("version", "")
            except Exception:
                os_version = ""
        if not os_version:
            if runtime_cfg.get("mode", "recovery") == "normal":
                os_version = getattr(config, "OS_VERSION", getattr(config, "FIRMWARE_VERSION", "unknown"))
            elif os_installed:
                os_version = "unknown"
        return {
            "runtime": runtime_version,
            "recovery": recovery_version,
            "os": os_version or "-",
        }

    def portal_status(self):
        network_cfg = self.config_store.load_network() if self.config_store is not None else {}
        runtime_cfg = self.config_store.load_runtime() if self.config_store is not None else {}
        selected_profile = self._selected_server_profile(runtime_cfg)
        master_server = dict(runtime_cfg.get("master_server", {}))
        data_server = dict(runtime_cfg.get("data_server", {}))
        profile_runtime = self._runtime_patch_for_server_profile(selected_profile)
        if profile_runtime is not None:
            if not master_server:
                master_server = dict(profile_runtime.get("master_server", {}))
            if not data_server:
                data_server = dict(profile_runtime.get("data_server", {}))
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
            "mode": runtime_cfg.get("mode", "recovery"),
            "os_installed": os_installed,
            "versions": self._version_status(runtime_cfg, os_installed),
            "release_url": self._github_release_url() or runtime_cfg.get("update", {}).get("release_url", ""),
            "ap_ssid": ap_ssid,
            "portal_ip": portal_ip,
            "portal_domain": portal_domain,
            "portal_url": self._portal_url(portal_domain or portal_ip),
            "portal_ip_url": self._portal_url(portal_ip),
            "saved_ssid": network_cfg.get("ssid", ""),
            "last_error": self.last_error,
            "last_setup_result": self.last_setup_result,
            "server_profile": selected_profile,
            "server_profile_options": self._server_profile_options(),
            "master_server": master_server,
            "data_server": data_server,
            "mqtt": dict(runtime_cfg.get("mqtt", {})),
            "transport": dict(runtime_cfg.get("transport", {})),
            "logging": dict(runtime_cfg.get("logging", {})),
        }

    def _server_profiles(self):
        profiles = getattr(config, "SERVER_PROFILES", {}) or {}
        normalized = {}
        for name, item in profiles.items():
            item = item or {}
            normalized[str(name)] = {
                "label": str(item.get("label", name)),
                "master_server": dict(item.get("master_server", {})),
                "data_server": dict(item.get("data_server", {})),
            }
        return normalized

    def _default_server_profile(self):
        profiles = self._server_profiles()
        default_name = getattr(config, "DEFAULT_SERVER_PROFILE", "")
        if default_name in profiles:
            return default_name
        for name in profiles:
            return name
        return ""

    def _runtime_patch_for_server_profile(
        self,
        profile_name,
        master_host="",
        master_port="",
        data_host="",
        data_port="",
    ):
        master_host = self._normalize_server_host(master_host)
        data_host = self._normalize_server_host(data_host)
        master_port = self._normalize_server_port(master_port)
        data_port = self._normalize_server_port(data_port)
        profile_name = str(profile_name or "").strip()
        profiles = self._server_profiles()
        if not profiles:
            return None
        if not profile_name:
            if master_host or data_host:
                profile_name = "manual" if "manual" in profiles else self._default_server_profile()
            else:
                return None
        if profile_name not in profiles:
            profile_name = self._default_server_profile()
        profile = profiles.get(profile_name, {})
        patch = {
            "server_profile": profile_name,
            "master_server": dict(profile.get("master_server", {})),
            "data_server": dict(profile.get("data_server", {})),
        }
        if profile_name == "manual":
            current_runtime = self.config_store.load_runtime() if self.config_store is not None else {}
            current_master = current_runtime.get("master_server", {})
            current_data = current_runtime.get("data_server", {})
            patch["master_server"]["port"] = master_port or int(
                current_master.get("port", patch["master_server"].get("port", 0)) or 0
            )
            patch["data_server"]["port"] = data_port or int(
                current_data.get("port", patch["data_server"].get("port", 0)) or 0
            )
            resolved_master = master_host or str(current_master.get("host", "") or patch["master_server"].get("host", "")).strip()
            resolved_data = data_host or str(current_data.get("host", "") or patch["data_server"].get("host", "")).strip()
            if resolved_master and not resolved_data:
                resolved_data = resolved_master
            if resolved_data and not resolved_master:
                resolved_master = resolved_data
            patch["master_server"]["host"] = resolved_master
            patch["data_server"]["host"] = resolved_data
        return patch

    def _selected_server_profile(self, runtime_cfg):
        runtime_cfg = runtime_cfg or {}
        profiles = self._server_profiles()
        requested = str(runtime_cfg.get("server_profile", "") or "").strip()
        if requested in profiles:
            return requested
        master = runtime_cfg.get("master_server", {})
        data = runtime_cfg.get("data_server", {})
        for name, profile in profiles.items():
            if self._server_equals(master, profile.get("master_server", {})) and self._server_equals(
                    data, profile.get("data_server", {})):
                return name
        return self._default_server_profile()

    def _runtime_patch_for_transport(self, profile_name="", mqtt_host="", mqtt_port=""):
        current_runtime = self.config_store.load_runtime() if self.config_store is not None else {}
        current_transport = dict(current_runtime.get("transport", {}))
        defaults = self._mqtt_defaults_for_profile(profile_name)
        normalized_host = self._normalize_server_host(mqtt_host)
        normalized_port = self._normalize_server_port(mqtt_port)
        if str(profile_name or "").strip() != "manual":
            normalized_host = ""
            normalized_port = 0
        return {
            "mqtt": {
                "host": normalized_host or defaults["host"],
                "port": normalized_port or defaults["port"],
                "tls": defaults["tls"],
            },
            "transport": {
                "mode": "mqtt",
                "topic_namespace": current_transport.get("topic_namespace", "newhorizons/v1"),
            },
        }

    def _mqtt_defaults_for_profile(self, profile_name):
        profile_name = str(profile_name or "").strip()
        if profile_name == "production":
            return {
                "host": getattr(config, "PRODUCTION_MQTT_HOST", getattr(config, "PRODUCTION_SERVER_HOST", "")),
                "port": int(getattr(config, "PRODUCTION_MQTT_PORT", 8883)),
                "tls": bool(getattr(config, "PRODUCTION_MQTT_TLS", True)),
            }
        return {
            "host": getattr(config, "MQTT_BROKER_HOST", "192.168.1.153"),
            "port": int(getattr(config, "MQTT_BROKER_PORT", 1883)),
            "tls": bool(getattr(config, "MQTT_TLS", False)),
        }

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

    def _server_profile_options(self):
        options = []
        for name, profile in self._server_profiles().items():
            options.append({
                "value": name,
                "label": profile.get("label", name),
                "master_host": profile.get("master_server", {}).get("host", ""),
                "data_host": profile.get("data_server", {}).get("host", ""),
            })
        return options

    def _server_equals(self, lhs, rhs):
        lhs = lhs or {}
        rhs = rhs or {}
        return (
            str(lhs.get("host", "")) == str(rhs.get("host", ""))
            and int(lhs.get("port", 0) or 0) == int(rhs.get("port", 0) or 0)
        )

    def _normalize_server_host(self, value):
        return str(value or "").strip()

    def _normalize_server_port(self, value):
        try:
            port = int(str(value or "").strip())
        except (TypeError, ValueError):
            return 0
        return port if 0 < port <= 65535 else 0

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
