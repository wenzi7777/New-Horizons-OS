FIRMWARE_NAME = "New Horizons OS"
FIRMWARE_VERSION = "v0.1.17"
DEFAULT_CHANNEL = "minimal"

ACTION_BUTTON_PIN = 46
BOOT_WINDOW_MS = 3000
BOOT_WINDOW_POLL_MS = 50

SETUP_AP_SSID_PREFIX = "NewHorizonsOS"
SETUP_AP_PASSWORD = "newhorizons"
SETUP_PORTAL_DOMAIN = "newhorizons.os"
SETUP_PORTAL_HOST = "192.168.4.1"
SETUP_PORTAL_PORT = 80
SETUP_PORTAL_TITLE = "New Horizons OS Wi-Fi Setup"

DEVICE_STATE_DIR = "device_state"
CALIBRATION_DIR = DEVICE_STATE_DIR + "/calibration"
LOG_PATH = DEVICE_STATE_DIR + "/logs/device.log"

DEFAULT_MASTER_HOST = "192.168.1.153"
DEFAULT_MASTER_PORT = 22345
DEFAULT_DATA_HOST = "192.168.1.153"
DEFAULT_DATA_PORT = 5005
PRODUCTION_SERVER_HOST = "isensing-s1.u-aizu.ac.jp"
DEFAULT_SERVER_PROFILE = "manual"
SERVER_PROFILES = {
    "manual": {
        "label": "Manual",
        "master_server": {"host": DEFAULT_MASTER_HOST, "port": DEFAULT_MASTER_PORT},
        "data_server": {"host": DEFAULT_DATA_HOST, "port": DEFAULT_DATA_PORT},
    },
    "production": {
        "label": "Production",
        "master_server": {"host": PRODUCTION_SERVER_HOST, "port": DEFAULT_MASTER_PORT},
        "data_server": {"host": PRODUCTION_SERVER_HOST, "port": DEFAULT_DATA_PORT},
    },
}
DEFAULT_MQTT_HOST = "isensing-s1.u-aizu.ac.jp"
DEFAULT_MQTT_PORT = 8883
DEFAULT_MQTT_TLS = True
DEFAULT_MQTT_USERNAME = ""
DEFAULT_MQTT_PASSWORD = ""
DEFAULT_TOPIC_NAMESPACE = "newhorizons/v1"
DEFAULT_SERVER_BASE_URL = "https://isensing-s1.u-aizu.ac.jp/newhorizons/ota"
DEFAULT_NTP_SERVERS = ["pool.ntp.org", "time.nist.gov"]
DEFAULT_BUFFER_FRAMES = 8
DEFAULT_TARGET_FPS = 60

DEFAULT_CONTROL_PORT = 22345
STATUS_ANNOUNCE_INTERVAL_MS = 2000

DEFAULT_MANIFESTS = {
    "minimal": "https://raw.githubusercontent.com/wenzi7777/New-Horizons-OS/v0.1.17/device/channels/minimal/manifest.json",
    "full": "https://raw.githubusercontent.com/wenzi7777/New-Horizons-OS/v0.1.17/device/channels/full/manifest.json",
}
DEFAULT_SERVER_MANIFESTS = {
    "minimal": DEFAULT_SERVER_BASE_URL + "/channels/minimal/manifest.json",
    "full": DEFAULT_SERVER_BASE_URL + "/channels/full/manifest.json",
}
