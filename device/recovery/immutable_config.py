FIRMWARE_NAME = "New Horizons OS"
HARDWARE_MODEL = "VD-CTL/R v1.0.F 2026.4"
RUNTIME_VERSION = "v0.2.28"
RECOVERY_VERSION = "v0.2.29"
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
DEFAULT_SERVER_HOST = "192.168.1.153"
DEFAULT_TCP_CONTROL_PORT = 22345
DEFAULT_UDP_STREAM_PORT = 13250
PRODUCTION_TCP_CONTROL_PORT = 22345
PRODUCTION_UDP_STREAM_PORT = 13250
SERVER_PROFILES = {
    "manual": {
        "label": "Manual",
        "server": {"host": DEFAULT_SERVER_HOST, "tcp_port": DEFAULT_TCP_CONTROL_PORT, "udp_port": DEFAULT_UDP_STREAM_PORT},
    },
    "production": {
        "label": "Production",
        "server": {"host": PRODUCTION_SERVER_HOST, "tcp_port": PRODUCTION_TCP_CONTROL_PORT, "udp_port": PRODUCTION_UDP_STREAM_PORT},
    },
}
DEFAULT_RELEASE_URL = "https://raw.githubusercontent.com/wenzi7777/New-Horizons-OS/main/releases/latest.json"
DEFAULT_NTP_SERVERS = ["pool.ntp.org", "time.nist.gov"]
DEFAULT_BUFFER_FRAMES = 2
DEFAULT_TARGET_FPS = 60

STATUS_ANNOUNCE_INTERVAL_MS = 2000

DEFAULT_MANIFESTS = {
    "recovery": "https://raw.githubusercontent.com/wenzi7777/New-Horizons-OS/v0.2.29/device/recovery/manifest.json",
    "os": "https://raw.githubusercontent.com/wenzi7777/New-Horizons-OS/v0.2.29/device/os/manifest.json",
}
