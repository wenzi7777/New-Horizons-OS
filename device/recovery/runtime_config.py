import immutable_config as iconfig
import storage


DEFAULT_SERVER = {
    "host": getattr(iconfig, "DEFAULT_SERVER_HOST", ""),
    "udp_port": int(getattr(iconfig, "DEFAULT_UDP_STREAM_PORT", 13250)),
    "source": "findme",
    "gateway_id": "",
}


DEFAULT_RUNTIME = {
    "firmware_name": iconfig.FIRMWARE_NAME,
    "mode": iconfig.DEFAULT_MODE,
    "packet_format_version": 2,
    "sensor_precision": "float32",
    "scan_timing": {
        "target_fps": iconfig.DEFAULT_TARGET_FPS,
        "send_every_n_frames": 1,
        "settle_us": 20,
        "core_id": 1,
    },
    "matrix_layout": {
        "active_rows": [],
        "active_cols": [],
    },
    "buffer_frames": iconfig.DEFAULT_BUFFER_FRAMES,
    "ntp_servers": list(iconfig.DEFAULT_NTP_SERVERS),
    "transport": {
        "mode": "udp",
    },
    "logging": {
        "enabled": True,
        "capacity": "default",
        "serial": "status",
    },
    "server": {
        "host": DEFAULT_SERVER["host"],
        "udp_port": DEFAULT_SERVER["udp_port"],
        "source": "findme",
        "gateway_id": DEFAULT_SERVER["gateway_id"],
    },
    "findme": {
        "enabled": True,
        "port": int(getattr(iconfig, "DEFAULT_GATEWAY_DISCOVERY_PORT", 22346)),
        "state": "idle",
        "gateway_id": "",
        "gateway_name": "",
        "host": "",
        "udp_port": int(getattr(iconfig, "DEFAULT_UDP_STREAM_PORT", 13250)),
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
        "manifest_url": iconfig.DEFAULT_MANIFESTS["recovery"],
        "release_url": iconfig.DEFAULT_RELEASE_URL,
        "enabled": True,
        "check_on_boot": True,
        "auto_apply": False,
        "source": "github",
        "sources": {
            "github": dict(iconfig.DEFAULT_MANIFESTS),
        },
    },
}

DEFAULT_NETWORK = {
    "wifi_mode": "STA",
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
    def __init__(self, base_dir=iconfig.DEVICE_STATE_DIR):
        self.base_dir = base_dir
        self.runtime_path = self.base_dir + "/runtime_config.tlv"
        self.network_path = self.base_dir + "/network_config.tlv"
        self.filter_path = self.base_dir + "/filter_config.tlv"
        self.update_state_path = self.base_dir + "/update_state.tlv"

    def _merged(self, base, override):
        result = {}
        for key, value in base.items():
            result[key] = dict(value) if isinstance(value, dict) else list(value) if isinstance(value, list) else value
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
        return self._merged(DEFAULT_RUNTIME, storage.load_tlv(self.runtime_path, {}))

    def save_runtime(self, runtime):
        storage.save_tlv(self.runtime_path, runtime)

    def update_runtime(self, patch):
        runtime = self._merged(self.load_runtime(), patch)
        self.save_runtime(runtime)
        return runtime

    def load_network(self):
        return self._merged(DEFAULT_NETWORK, storage.load_tlv(self.network_path, {}))

    def save_network(self, network_data):
        storage.save_tlv(self.network_path, network_data)

    def update_network(self, patch):
        network_data = self._merged(self.load_network(), patch)
        self.save_network(network_data)
        return network_data

    def load_filter(self):
        return self._merged(DEFAULT_FILTER, storage.load_tlv(self.filter_path, {}))

    def save_filter(self, filter_data):
        storage.save_tlv(self.filter_path, filter_data)

    def load_update_state(self):
        state = self._merged({
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
        }, storage.load_tlv(self.update_state_path, {}))
        self.save_update_state(state)
        return state

    def save_update_state(self, state):
        storage.save_tlv(self.update_state_path, state)
