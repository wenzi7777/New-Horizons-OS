import config
import storage


DEFAULT_SERVER = {
    "host": getattr(config, "DEFAULT_SERVER_HOST", ""),
    "tcp_port": int(getattr(config, "DEFAULT_TCP_CONTROL_PORT", 22345)),
    "udp_port": int(getattr(config, "DEFAULT_UDP_STREAM_PORT", 13250)),
    "source": "findme",
    "gateway_id": "",
}
OS_GITHUB_BASE_URL = getattr(config, "GITHUB_BASE_URL", "")
RECOVERY_GITHUB_BASE_URL = getattr(config, "RECOVERY_GITHUB_BASE_URL", OS_GITHUB_BASE_URL)


DEFAULT_RUNTIME = {
    "firmware_name": "New Horizons OS",
    "mode": "normal",
    "packet_format_version": config.PACKET_VERSION,
    "sensor_precision": "float32",
    "scan_timing": {
        "target_fps": config.TARGET_FPS,
        "send_every_n_frames": config.SEND_EVERY_N_FRAMES,
        "settle_us": config.MATRIX_SETTLE_US,
        "core_id": 1,
    },
    "matrix_layout": {
        "active_rows": [],
        "active_cols": [],
    },
    "matrix_layout_state": {
        "pending": False,
        "committed": False,
        "last_error": "",
    },
    "matrix_scan_state": {
        "active": False,
        "autostart_disabled": False,
        "last_error": "",
    },
    "resource_state": "normal",
    "stream_state": {
        "state": "normal",
        "cooldown_ms": 0,
        "last_error": "",
        "failed_sends": 0,
    },
    "buffer_frames": 2,
    "ntp_servers": ["pool.ntp.org", "time.nist.gov"],
    "transport": {
        "mode": "udp",
    },
    "indicators": {
        "external_led": {
            "mode": "off",
            "manual_preset": "stream_health",
            "brightness": float(getattr(config, "EXTERNAL_LED_DEFAULT_BRIGHTNESS", 0.35)),
        },
        "oled": {
            "mode": "off",
            "page": getattr(config, "OLED_DEFAULT_PAGE", "live_status"),
            "update_hz": int(getattr(config, "OLED_DEFAULT_UPDATE_HZ", 1)),
            "contrast": int(getattr(config, "OLED_DEFAULT_CONTRAST", 128)),
        },
    },
    "logging": {
        "enabled": True,
        "capacity": "default",
        "serial": "status",
    },
    "server": {
        "host": DEFAULT_SERVER["host"],
        "tcp_port": DEFAULT_SERVER["tcp_port"],
        "udp_port": DEFAULT_SERVER["udp_port"],
        "source": "findme",
        "gateway_id": DEFAULT_SERVER["gateway_id"],
    },
    "findme": {
        "enabled": True,
        "port": int(getattr(config, "DEFAULT_GATEWAY_DISCOVERY_PORT", 22346)),
        "state": "idle",
        "gateway_id": "",
        "gateway_name": "",
        "host": "",
        "tcp_port": int(getattr(config, "DEFAULT_TCP_CONTROL_PORT", 22345)),
        "udp_port": int(getattr(config, "DEFAULT_UDP_STREAM_PORT", 13250)),
        "last_success_ms": 0,
        "last_error": "",
        "source": "findme",
        "rejected_gateways": [],
        "preferred_gateway_id": "",
        "claim_id": "",
        "claim_expires_at_ms": 0,
        "last_claim_error": "",
    },
    "update": {
        "manifest_url": "",
        "release_url": getattr(config, "DEFAULT_RELEASE_URL", getattr(config, "GITHUB_RELEASE_URL", "")),
        "enabled": False,
        "check_on_boot": False,
        "auto_apply": False,
        "source": "github",
        "sources": {
            "github": {
                "recovery": RECOVERY_GITHUB_BASE_URL + "/recovery/manifest.json",
                "os": OS_GITHUB_BASE_URL + "/os/manifest.json",
            },
        },
    },
}


DEFAULT_NETWORK = {
    "wifi_mode": config.WIFI_MODE,
    "ssid": "",
    "password": "",
    "setup_method": "softap_webui",
    "last_ssid": "",
}


DEFAULT_FILTER = {
    "enabled": False,
    "median": 3,
    "alpha": 0.25,
}


class RuntimeConfigStore:
    def __init__(self, base_dir="device_state"):
        self.base_dir = base_dir
        self.runtime_path = self.base_dir + "/runtime_config.json"
        self.network_path = self.base_dir + "/network_config.json"
        self.filter_path = self.base_dir + "/filter_config.json"
        self.update_state_path = self.base_dir + "/update_state.json"

    def _merged(self, base, override):
        result = {}
        for key, value in base.items():
            if isinstance(value, dict):
                result[key] = dict(value)
            elif isinstance(value, list):
                result[key] = list(value)
            else:
                result[key] = value

        if not override:
            return result

        for key, value in override.items():
            if isinstance(value, dict) and isinstance(result.get(key), dict):
                merged = dict(result[key])
                merged.update(value)
                result[key] = merged
            else:
                result[key] = value
        return result

    def load_runtime(self):
        data = storage.load_json(self.runtime_path, {})
        return self._merged(DEFAULT_RUNTIME, data)

    def save_runtime(self, runtime):
        storage.save_json(self.runtime_path, runtime)

    def update_runtime(self, patch):
        runtime = self.load_runtime()
        runtime = self._merged(runtime, patch)
        self.save_runtime(runtime)
        return runtime

    def load_network(self):
        data = storage.load_json(self.network_path, {})
        return self._merged(DEFAULT_NETWORK, data)

    def save_network(self, network_data):
        storage.save_json(self.network_path, network_data)

    def update_network(self, patch):
        network_data = self.load_network()
        network_data = self._merged(network_data, patch)
        self.save_network(network_data)
        return network_data

    def load_filter(self):
        data = storage.load_json(self.filter_path, {})
        return self._merged(DEFAULT_FILTER, data)

    def save_filter(self, filter_data):
        storage.save_json(self.filter_path, filter_data)

    def update_filter(self, patch):
        filter_data = self.load_filter()
        filter_data = self._merged(filter_data, patch)
        self.save_filter(filter_data)
        return filter_data

    def load_update_state(self):
        default = {
            "last_manifest_sha256": "",
            "last_check_ms": 0,
            "last_result": "",
            "reboot_required": False,
            "phase": "idle",
            "operation": "",
            "total_files": 0,
            "applied_files": 0,
            "current_file": "",
            "last_error": "",
            "started_at_ms": 0,
            "finished_at_ms": 0,
        }
        state = storage.load_json(self.update_state_path, {})
        state = self._merged(default, state)
        self.save_update_state(state)
        return state

    def save_update_state(self, state):
        storage.save_json(self.update_state_path, state)
