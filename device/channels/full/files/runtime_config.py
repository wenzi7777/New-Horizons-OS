import config
import storage


DEFAULT_RUNTIME = {
    "firmware_name": "New Horizons OS",
    "channel": "full",
    "master_server": {
        "host": config.UDP_SERVER_IP,
        "port": 22345,
    },
    "data_server": {
        "host": config.UDP_SERVER_IP,
        "port": config.UDP_SERVER_PORT,
    },
    "packet_format_version": config.PACKET_VERSION,
    "sensor_precision": "float32",
    "scan_timing": {
        "target_fps": config.TARGET_FPS,
        "send_every_n_frames": config.SEND_EVERY_N_FRAMES,
        "settle_us": config.MATRIX_SETTLE_US,
        "core_id": 1,
    },
    "buffer_frames": 8,
    "ntp_servers": ["pool.ntp.org", "time.nist.gov"],
    "update": {
        "manifest_url": "",
        "enabled": False,
        "check_on_boot": True,
        "auto_apply": False,
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
    def __init__(self, base_dir=".device"):
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
        runtime = self._merged(DEFAULT_RUNTIME, data)
        self.save_runtime(runtime)
        return runtime

    def save_runtime(self, runtime):
        storage.save_json(self.runtime_path, runtime)

    def update_runtime(self, patch):
        runtime = self.load_runtime()
        runtime = self._merged(runtime, patch)
        self.save_runtime(runtime)
        return runtime

    def load_network(self):
        data = storage.load_json(self.network_path, {})
        network = self._merged(DEFAULT_NETWORK, data)
        self.save_network(network)
        return network

    def save_network(self, network_data):
        storage.save_json(self.network_path, network_data)

    def update_network(self, patch):
        network_data = self.load_network()
        network_data = self._merged(network_data, patch)
        self.save_network(network_data)
        return network_data

    def load_filter(self):
        data = storage.load_json(self.filter_path, {})
        filt = self._merged(DEFAULT_FILTER, data)
        self.save_filter(filt)
        return filt

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
