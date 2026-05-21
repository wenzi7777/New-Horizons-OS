DEVICE_NAME = "New Horizons OS"
WIFI_MODE = "STA"
SETUP_AP_SSID_PREFIX = "NewHorizonsOS"
SETUP_AP_PASSWORD = "newhorizons"
SETUP_PORTAL_DOMAIN = "newhorizons.os"
SETUP_PORTAL_HOST = "192.168.4.1"
SETUP_PORTAL_PORT = 80
SETUP_PORTAL_TITLE = "New Horizons OS Wi-Fi Setup"
PRODUCTION_SERVER_HOST = "isensing-s1.u-aizu.ac.jp"
DEFAULT_SERVER_HOST = "192.168.1.153"
DEFAULT_TCP_CONTROL_PORT = 22345
DEFAULT_UDP_STREAM_PORT = 13250
PRODUCTION_TCP_CONTROL_PORT = 22345
PRODUCTION_UDP_STREAM_PORT = 13250
GITHUB_RELEASE_URL = "https://raw.githubusercontent.com/wenzi7777/New-Horizons-OS/main/releases/latest.json"
DEFAULT_RELEASE_URL = GITHUB_RELEASE_URL
DEFAULT_SERVER_PROFILE = "production"
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
DEVICE_STATE_DIR = "device_state"
OS_DIR = "nhos"
CALIBRATION_DIR = DEVICE_STATE_DIR + "/calibration"
LOG_PATH = DEVICE_STATE_DIR + "/logs/device.log"
TARGET_FPS = 60
SEND_EVERY_N_FRAMES = 1
MATRIX_SETTLE_US = 20
PACKET_VERSION = 2
ENABLE_IMU = False
ENABLE_BATTERY = False
ENABLE_LED = False
PRINT_WIFI_STATUS = False
