import immutable_config as iconfig
import storage


DEFAULT_RUNTIME = {
    "firmware_name": iconfig.FIRMWARE_NAME,
    "channel": iconfig.DEFAULT_CHANNEL,
    "master_server": {"host": iconfig.DEFAULT_MASTER_HOST, "port": iconfig.DEFAULT_MASTER_PORT},
    "data_server": {"host": iconfig.DEFAULT_DATA_HOST, "port": iconfig.DEFAULT_DATA_PORT},
    "packet_format_version": 1,
    "sensor_precision": "float32",
    "scan_timing": {
        "target_fps": iconfig.DEFAULT_TARGET_FPS,
        "send_every_n_frames": 1,
        "settle_us": 20,
        "core_id": 1,
    },
    "buffer_frames": iconfig.DEFAULT_BUFFER_FRAMES,
    "ntp_servers": list(iconfig.DEFAULT_NTP_SERVERS),
    "update": {
        "manifest_url": iconfig.DEFAULT_MANIFESTS[iconfig.DEFAULT_CHANNEL],
        "enabled": True,
        "check_on_boot": True,
        "auto_apply": False,
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
        self.runtime_path = self.base_dir + "/runtime_config.json"
        self.network_path = self.base_dir + "/network_config.json"
        self.filter_path = self.base_dir + "/filter_config.json"
        self.update_state_path = self.base_dir + "/update_state.json"

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
        runtime = self._merged(DEFAULT_RUNTIME, storage.load_json(self.runtime_path, {}))
        self.save_runtime(runtime)
        return runtime

    def save_runtime(self, runtime):
        storage.save_json(self.runtime_path, runtime)

    def update_runtime(self, patch):
        runtime = self._merged(self.load_runtime(), patch)
        self.save_runtime(runtime)
        return runtime

    def load_network(self):
        network = self._merged(DEFAULT_NETWORK, storage.load_json(self.network_path, {}))
        self.save_network(network)
        return network

    def save_network(self, network_data):
        storage.save_json(self.network_path, network_data)

    def update_network(self, patch):
        network_data = self._merged(self.load_network(), patch)
        self.save_network(network_data)
        return network_data

    def load_filter(self):
        filt = self._merged(DEFAULT_FILTER, storage.load_json(self.filter_path, {}))
        self.save_filter(filt)
        return filt

    def save_filter(self, filter_data):
        storage.save_json(self.filter_path, filter_data)

    def load_update_state(self):
        state = self._merged({
            "last_manifest_sha256": "",
            "last_check_ms": 0,
            "last_result": "",
            "reboot_required": False,
        }, storage.load_json(self.update_state_path, {}))
        self.save_update_state(state)
        return state

    def save_update_state(self, state):
        storage.save_json(self.update_state_path, state)
