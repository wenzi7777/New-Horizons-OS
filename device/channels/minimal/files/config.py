DEVICE_NAME = "New Horizons OS"
WIFI_MODE = "STA"
SETUP_AP_SSID_PREFIX = "NewHorizonsOS"
SETUP_AP_PASSWORD = "newhorizons"
SETUP_PORTAL_DOMAIN = "newhorizons.os"
SETUP_PORTAL_HOST = "192.168.4.1"
SETUP_PORTAL_PORT = 80
SETUP_PORTAL_TITLE = "New Horizons OS Wi-Fi Setup"
UDP_SERVER_IP = "192.168.1.153"
UDP_SERVER_PORT = 5005
UDP_CONTROL_PORT = 22345
PRODUCTION_SERVER_HOST = "isensing-s1.u-aizu.ac.jp"
DEFAULT_SERVER_PROFILE = "manual"
SERVER_PROFILES = {
    "manual": {
        "label": "Manual",
        "master_server": {"host": UDP_SERVER_IP, "port": UDP_CONTROL_PORT},
        "data_server": {"host": UDP_SERVER_IP, "port": UDP_SERVER_PORT},
    },
    "production": {
        "label": "Production",
        "master_server": {"host": PRODUCTION_SERVER_HOST, "port": UDP_CONTROL_PORT},
        "data_server": {"host": PRODUCTION_SERVER_HOST, "port": UDP_SERVER_PORT},
    },
}
DEVICE_STATE_DIR = "device_state"
CALIBRATION_DIR = DEVICE_STATE_DIR + "/calibration"
LOG_PATH = DEVICE_STATE_DIR + "/logs/device.log"
TARGET_FPS = 60
SEND_EVERY_N_FRAMES = 1
MATRIX_SETTLE_US = 20
PACKET_VERSION = 1
ENABLE_IMU = False
ENABLE_BATTERY = False
ENABLE_LED = False
PRINT_WIFI_STATUS = True
