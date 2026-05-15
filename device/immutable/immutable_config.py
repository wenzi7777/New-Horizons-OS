FIRMWARE_NAME = "New Horizons OS"
FIRMWARE_VERSION = "v0.1.12"
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
DEFAULT_NTP_SERVERS = ["pool.ntp.org", "time.nist.gov"]
DEFAULT_BUFFER_FRAMES = 8
DEFAULT_TARGET_FPS = 60

DEFAULT_CONTROL_PORT = 22345
STATUS_ANNOUNCE_INTERVAL_MS = 2000

DEFAULT_MANIFESTS = {
    "minimal": "https://raw.githubusercontent.com/wenzi7777/New-Horizons-OS/v0.1.12/device/channels/minimal/manifest.json",
    "full": "https://raw.githubusercontent.com/wenzi7777/New-Horizons-OS/v0.1.12/device/channels/full/manifest.json",
}
