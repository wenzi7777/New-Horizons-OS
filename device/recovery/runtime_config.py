import immutable_config as iconfig
import storage


def _server_profiles():
    return getattr(iconfig, "SERVER_PROFILES", {}) or {}


def _default_server_profile_name():
    default_name = getattr(iconfig, "DEFAULT_SERVER_PROFILE", "")
    profiles = _server_profiles()
    if default_name in profiles:
        return default_name
    for name in profiles:
        return name
    return ""


DEFAULT_SERVER_PROFILE = _default_server_profile_name()


def _mqtt_defaults_for_profile(profile_name):
    profile = _server_profiles().get(profile_name, {})
    mqtt_cfg = profile.get("mqtt", {})
    if mqtt_cfg:
        return {
            "host": mqtt_cfg.get("host", ""),
            "port": int(mqtt_cfg.get("port", 8883)),
            "tls": bool(mqtt_cfg.get("tls", True)),
        }
    if profile_name == "production":
        return {
            "host": getattr(iconfig, "PRODUCTION_MQTT_HOST", getattr(iconfig, "PRODUCTION_SERVER_HOST", "")),
            "port": int(getattr(iconfig, "PRODUCTION_MQTT_PORT", 8883)),
            "tls": bool(getattr(iconfig, "PRODUCTION_MQTT_TLS", True)),
        }
    return {
        "host": iconfig.DEFAULT_MQTT_HOST,
        "port": iconfig.DEFAULT_MQTT_PORT,
        "tls": iconfig.DEFAULT_MQTT_TLS,
    }


DEFAULT_MQTT = _mqtt_defaults_for_profile(DEFAULT_SERVER_PROFILE)


DEFAULT_RUNTIME = {
    "firmware_name": iconfig.FIRMWARE_NAME,
    "mode": iconfig.DEFAULT_MODE,
    "server_profile": DEFAULT_SERVER_PROFILE,
    "packet_format_version": 1,
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
        "mode": "mqtt",
        "topic_namespace": iconfig.DEFAULT_TOPIC_NAMESPACE,
    },
    "logging": {
        "enabled": True,
        "capacity": "default",
        "serial": "status",
    },
    "mqtt": {
        "host": DEFAULT_MQTT["host"],
        "port": DEFAULT_MQTT["port"],
        "tls": DEFAULT_MQTT["tls"],
        "username": iconfig.DEFAULT_MQTT_USERNAME,
        "password": iconfig.DEFAULT_MQTT_PASSWORD,
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
        return self._merged(DEFAULT_RUNTIME, storage.load_json(self.runtime_path, {}))

    def save_runtime(self, runtime):
        storage.save_json(self.runtime_path, runtime)

    def update_runtime(self, patch):
        runtime = self._merged(self.load_runtime(), patch)
        self.save_runtime(runtime)
        return runtime

    def load_network(self):
        return self._merged(DEFAULT_NETWORK, storage.load_json(self.network_path, {}))

    def save_network(self, network_data):
        storage.save_json(self.network_path, network_data)

    def update_network(self, patch):
        network_data = self._merged(self.load_network(), patch)
        self.save_network(network_data)
        return network_data

    def load_filter(self):
        return self._merged(DEFAULT_FILTER, storage.load_json(self.filter_path, {}))

    def save_filter(self, filter_data):
        storage.save_json(self.filter_path, filter_data)

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
        }, storage.load_json(self.update_state_path, {}))
        self.save_update_state(state)
        return state

    def save_update_state(self, state):
        storage.save_json(self.update_state_path, state)
