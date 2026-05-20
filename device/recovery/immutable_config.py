FIRMWARE_NAME = "New Horizons OS"
HARDWARE_MODEL = "VD-CTL/R v1.0.F 2026.4"
RUNTIME_VERSION = "v0.2.19"
RECOVERY_VERSION = "v0.2.25"
FIRMWARE_VERSION = RECOVERY_VERSION
DEFAULT_MODE = "recovery"

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
RECOVERY_DIR = "recovery"
OS_DIR = "nhos"
OTA_STAGE_DIR = "ota_stage"
CALIBRATION_DIR = DEVICE_STATE_DIR + "/calibration"
DATA_FILES_DIR = "data/files"
DATA_LOG_DIR = "data/logs"
DATA_TMP_DIR = "data/tmp"
LOG_PATH = DATA_LOG_DIR + "/device.log"

PRODUCTION_SERVER_HOST = "isensing-s1.u-aizu.ac.jp"
DEFAULT_SERVER_PROFILE = "production"
DEFAULT_MQTT_HOST = "192.168.1.153"
DEFAULT_MQTT_PORT = 1883
DEFAULT_MQTT_TLS = False
PRODUCTION_MQTT_HOST = PRODUCTION_SERVER_HOST
PRODUCTION_MQTT_PORT = 8883
PRODUCTION_MQTT_TLS = True
SERVER_PROFILES = {
    "manual": {
        "label": "Manual",
        "mqtt": {"host": DEFAULT_MQTT_HOST, "port": DEFAULT_MQTT_PORT, "tls": DEFAULT_MQTT_TLS},
    },
    "production": {
        "label": "Production",
        "mqtt": {"host": PRODUCTION_MQTT_HOST, "port": PRODUCTION_MQTT_PORT, "tls": PRODUCTION_MQTT_TLS},
    },
}
DEFAULT_MQTT_USERNAME = ""
DEFAULT_MQTT_PASSWORD = ""
DEFAULT_TOPIC_NAMESPACE = "newhorizons/v1"
DEFAULT_RELEASE_URL = "https://raw.githubusercontent.com/wenzi7777/New-Horizons-OS/main/releases/latest.json"
DEFAULT_NTP_SERVERS = ["pool.ntp.org", "time.nist.gov"]
DEFAULT_BUFFER_FRAMES = 8
DEFAULT_TARGET_FPS = 60

STATUS_ANNOUNCE_INTERVAL_MS = 2000

DEFAULT_MANIFESTS = {
    "recovery": "https://raw.githubusercontent.com/wenzi7777/New-Horizons-OS/v0.2.25/device/recovery/manifest.json",
    "os": "https://raw.githubusercontent.com/wenzi7777/New-Horizons-OS/v0.2.25/device/os/manifest.json",
}
