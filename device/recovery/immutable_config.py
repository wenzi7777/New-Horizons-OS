FIRMWARE_NAME = "New Horizons OS"
HARDWARE_MODEL = "VD-CTL/R v1.0.F 2026.4"
RUNTIME_VERSION = "v0.3.0"
RECOVERY_VERSION = "v0.4.25"
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

DEFAULT_SERVER_HOST = ""
DEFAULT_UDP_STREAM_PORT = 13250
DEFAULT_GATEWAY_DISCOVERY_PORT = 22346
GATEWAY_DISCOVERY_TIMEOUT_MS = 1500
GATEWAY_DISCOVERY_ATTEMPTS = 2
GATEWAY_DISCOVERY_RETRY_MS = 5000
DEFAULT_RELEASE_URL = "https://raw.githubusercontent.com/wenzi7777/New-Horizons-OS/main/releases/latest.tlv"
DEFAULT_NTP_SERVERS = ["pool.ntp.org", "time.nist.gov"]
DEFAULT_BUFFER_FRAMES = 2
DEFAULT_TARGET_FPS = 60

STATUS_ANNOUNCE_INTERVAL_MS = 2000

DEFAULT_MANIFESTS = {
    "recovery": "https://raw.githubusercontent.com/wenzi7777/New-Horizons-OS/v0.4.25/device/recovery/manifest.tlv",
    "os": "https://raw.githubusercontent.com/wenzi7777/New-Horizons-OS/v0.4.25/device/os/manifest.tlv",
}
