# wifi_manager.py
import time
import gc
import network

import config
import secrets
from device_identity import get_device_suffix
from wifi_portal import WiFiSetupPortal


class WiFiManager:
    def __init__(self, config_store=None, logger=None):
        self.sta = None
        self.ap = None
        self.config_store = config_store
        self.logger = logger
        self.state = "idle"
        self.last_error = ""
        self.last_setup_result = ""
        self.portal = WiFiSetupPortal(self, config, logger)

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
            if config.PRINT_WIFI_STATUS:
                print("Wi-Fi already connected:", sta.ifconfig())
            self.state = "wifi_connected"
            return True

        if ssid is None:
            ssid, password = self._credentials()

        if not ssid:
            self.last_error = "missing_credentials"
            self.state = "wifi_failed"
            return False

        for attempt in range(1, 4):
            print("Wi-Fi connect attempt:", attempt)
            print("Free memory:", gc.mem_free())

            try:
                try:
                    sta.disconnect()
                    time.sleep_ms(300)
                except Exception:
                    pass

                if config.PRINT_WIFI_STATUS:
                    print("Connecting Wi-Fi:", ssid)

                sta.connect(ssid, password)

                for _ in range(20):
                    if sta.isconnected():
                        if config.PRINT_WIFI_STATUS:
                            print("Wi-Fi connected:", sta.ifconfig())
                        self.stop_setup_portal()
                        self._disable_ap()
                        self.state = "wifi_connected"
                        return True

                    if config.PRINT_WIFI_STATUS:
                        print("Waiting Wi-Fi...")
                    time.sleep(1)

                print("Wi-Fi connect timeout")

            except RuntimeError as e:
                print("Wi-Fi connect RuntimeError:", e)
                print("Free memory:", gc.mem_free())

            except OSError as e:
                print("Wi-Fi connect OSError:", e)
                print("Free memory:", gc.mem_free())

            self._reset_sta_interface()
            gc.collect()

        print("Wi-Fi connect failed after retries")
        self.last_error = "wifi_connect_failed"
        self.state = "wifi_failed"
        return False

    def _reset_sta_interface(self):
        print("Reset Wi-Fi STA interface")
        sta = self._ensure_sta()

        try:
            sta.active(False)
            time.sleep_ms(800)
        except Exception as e:
            print("Wi-Fi active(False) warning:", e)

        try:
            sta.active(True)
            time.sleep_ms(800)
        except Exception as e:
            print("Wi-Fi active(True) warning:", e)

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

        self.portal.start()
        if self.logger:
            self.logger.info("wifi_setup_ap_ssid={}".format(ap_ssid))
        print("AP started:", ap.ifconfig())
        return True

    def service_setup_portal(self):
        return self.portal.service()

    def stop_setup_portal(self):
        self.portal.stop()
        self._disable_ap()

    def setup_active(self):
        return bool(self.portal.active)

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

    def apply_credentials(self, ssid, password):
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

    def portal_status(self):
        network_cfg = self.config_store.load_network() if self.config_store is not None else {}
        portal_ip = config.SETUP_PORTAL_HOST
        portal_domain = getattr(config, "SETUP_PORTAL_DOMAIN", "")
        ap_ssid = self.ap_ssid()
        if self.ap is not None:
            try:
                portal_ip = self.ap.ifconfig()[0]
            except Exception:
                portal_ip = config.SETUP_PORTAL_HOST
        return {
            "active": self.setup_active(),
            "state": self.state,
            "ap_ssid": ap_ssid,
            "portal_ip": portal_ip,
            "portal_domain": portal_domain,
            "portal_url": self._portal_url(portal_domain or portal_ip),
            "portal_ip_url": self._portal_url(portal_ip),
            "saved_ssid": network_cfg.get("ssid", ""),
            "last_error": self.last_error,
            "last_setup_result": self.last_setup_result,
        }

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

    def _disable_ap(self):
        ap = self._ensure_ap()
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
