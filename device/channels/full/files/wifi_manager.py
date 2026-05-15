# wifi_manager.py
import time
import gc
import network

import config
import secrets


class WiFiManager:
    def __init__(self, config_store=None, logger=None):
        self.wlan = None
        self.config_store = config_store
        self.logger = logger
        self.state = "idle"

    def connect(self):
        wifi_mode = config.WIFI_MODE
        if self.config_store is not None:
            wifi_mode = self.config_store.load_network().get("wifi_mode", config.WIFI_MODE)

        if wifi_mode == "AP":
            return self.start_ap()
        return self.connect_sta()

    def connect_sta(self):
        gc.collect()
        self.state = "normal_boot"

        self.wlan = network.WLAN(network.STA_IF)
        self.wlan.active(True)
        time.sleep_ms(300)

        if self.wlan.isconnected():
            if config.PRINT_WIFI_STATUS:
                print("Wi-Fi already connected:", self.wlan.ifconfig())
            self.state = "wifi_connected"
            return True

        ssid, password = self._credentials()

        if not ssid:
            if config.PRINT_WIFI_STATUS:
                print("No explicit Wi-Fi credentials found, waiting for NVS auto-connect")
            for _ in range(10):
                if self.wlan.isconnected():
                    if config.PRINT_WIFI_STATUS:
                        print("Wi-Fi connected from stored credentials:", self.wlan.ifconfig())
                    self.state = "wifi_connected"
                    return True
                time.sleep(1)
            print("Wi-Fi auto-connect timeout")
            self.state = "wifi_failed"
            return False

        for attempt in range(1, 4):
            print("Wi-Fi connect attempt:", attempt)
            print("Free memory:", gc.mem_free())

            try:
                # 有些 MicroPython/ESP32 韌體在重複 connect 前需要 disconnect
                try:
                    self.wlan.disconnect()
                    time.sleep_ms(300)
                except Exception:
                    pass

                if config.PRINT_WIFI_STATUS:
                    print("Connecting Wi-Fi:", ssid)

                self.wlan.connect(ssid, password)

                for _ in range(20):
                    if self.wlan.isconnected():
                        if config.PRINT_WIFI_STATUS:
                            print("Wi-Fi connected:", self.wlan.ifconfig())
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

            # 失敗後重置 Wi-Fi interface，再試下一次
            self._reset_sta_interface()
            gc.collect()

        print("Wi-Fi connect failed after retries")
        self.state = "wifi_failed"
        return False

    def _reset_sta_interface(self):
        print("Reset Wi-Fi STA interface")

        try:
            self.wlan.active(False)
            time.sleep_ms(800)
        except Exception as e:
            print("Wi-Fi active(False) warning:", e)

        try:
            self.wlan.active(True)
            time.sleep_ms(800)
        except Exception as e:
            print("Wi-Fi active(True) warning:", e)

    def start_ap(self):
        gc.collect()
        self.state = "provision_active"

        self.wlan = network.WLAN(network.AP_IF)

        try:
            self.wlan.active(True)
            time.sleep_ms(300)
        except Exception:
            pass

        self.wlan.config(
            essid="VD-CTL-R",
            password="12345678"
        )

        print("AP started:", self.wlan.ifconfig())
        return True

    def is_connected(self):
        if self.wlan is None:
            return False
        return self.wlan.isconnected()

    def _credentials(self):
        ssid = secrets.WIFI_SSID
        password = secrets.WIFI_PASSWORD

        if self.config_store is not None:
            network_cfg = self.config_store.load_network()
            if network_cfg.get("ssid"):
                ssid = network_cfg.get("ssid")
                password = network_cfg.get("password", "")

        return ssid, password
