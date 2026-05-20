# config.py

DEVICE_ID = 0x00000001
DEVICE_NAME = "New Horizons OS"
HARDWARE_MODEL = "VD-CTL/R v1.0.F 2026.4"
RUNTIME_VERSION = "v0.2.28"
FIRMWARE_VERSION = "v0.2.29"
OS_VERSION = FIRMWARE_VERSION
RECOVERY_VERSION = "v0.2.29"
RECOVERY_FIRMWARE_VERSION = RECOVERY_VERSION
SETUP_AP_SSID_PREFIX = "NewHorizonsOS"
SETUP_AP_PASSWORD = "newhorizons"
SETUP_PORTAL_DOMAIN = "newhorizons.os"
SETUP_PORTAL_HOST = "192.168.4.1"
SETUP_PORTAL_PORT = 80
SETUP_PORTAL_TITLE = "New Horizons OS Wi-Fi Setup"

ROWS = 10
COLS = 21

# Available physical GPIO pins
# ROW pins are ADC inputs.
# COL pins are digital select outputs.
AVAILABLE_ROWS = [1, 2, 3, 4, 5, 6, 7, 8, 9, 10]
AVAILABLE_COLS = [
    13, 14, 15, 16, 17, 18, 19, 20, 21, 26,
    47, 33, 34, 48, 35, 36, 37, 38, 39, 40, 41
]

# Active physical GPIO pins.
# Example:
# GPIO1 is logical R0 because it is AVAILABLE_ROWS[0].
# GPIO13 is logical C0 because it is AVAILABLE_COLS[0].
ACTIVE_ROWS = [1, 2, 3, 4, 5, 6, 7, 8, 9, 10]
ACTIVE_COLS = [
    13, 14, 15, 16, 17, 18, 19, 20, 21,
    47, 33, 34, 48, 35, 36, 37, 38, 39, 40, 41
]

# None = len(ACTIVE_ROWS) * len(ACTIVE_COLS)
# Or set a number such as 75 to output only the first 75 active points.
ACTIVE_SENSOR_COUNT = None

TARGET_FPS = 60
MAX_FPS = 60

IMU_RATE_HZ = 30
BATTERY_RATE_HZ = 1
LED_RATE_HZ = 20

PRODUCTION_SERVER_HOST = "isensing-s1.u-aizu.ac.jp"
DEFAULT_SERVER_HOST = "192.168.1.153"
DEFAULT_TCP_CONTROL_PORT = 22345
DEFAULT_UDP_STREAM_PORT = 13250
PRODUCTION_TCP_CONTROL_PORT = 22345
PRODUCTION_UDP_STREAM_PORT = 13250
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
GITHUB_BASE_URL = "https://raw.githubusercontent.com/wenzi7777/New-Horizons-OS/v0.2.29/device"
RECOVERY_GITHUB_BASE_URL = "https://raw.githubusercontent.com/wenzi7777/New-Horizons-OS/v0.2.29/device"
GITHUB_RELEASE_URL = "https://raw.githubusercontent.com/wenzi7777/New-Horizons-OS/main/releases/latest.json"
DEFAULT_RELEASE_URL = GITHUB_RELEASE_URL
STATUS_ANNOUNCE_INTERVAL_MS = 2000

# Packet format:
# "BINARY" or "TEXT_LINE"
# TEXT_LINE is easier to debug but slower and uses more RAM.
# BINARY is recommended for 45~60Hz streaming.
PACKET_FORMAT = "BINARY"
# PACKET_FORMAT = "TEXT_LINE"

TEXT_END_MARKER = "EndOfLine"
TEXT_MATRIX_WITH_LABEL = True

# TEXT_LINE_HMAC = False means text packets do not append HMAC.
# For normal logging/debug, keep False.
TEXT_LINE_HMAC = False

# Binary packet HMAC
USE_HMAC = True
HMAC_LEN = 16

# Packet buffer
USE_PACKET_BUFFER = True

# Recommended 2~4. Larger buffers use more RAM.
PACKET_BUFFER_SIZE = 2

# True: if buffer is full, drop the oldest frame and keep the newest frame.
# This is better for real-time sensor data.
PACKET_BUFFER_DROP_OLDEST = True

# Maximum packets to send per main-loop iteration.
SEND_MAX_PER_LOOP = 1

# Stop hammering lwIP briefly after a failed UDP send. Stale sensor packets are
# not useful for realtime streaming, so the app drops them during this window.
SEND_FAILURE_BACKOFF_MS = 100

# Matrix layout changes are staged and only committed when the native scan core
# can produce a packet while leaving enough heap headroom for control/status work.
SCAN_HEALTH_PROBE_MS = 2000
SCAN_MIN_HEAP_FREE = 16384
SCAN_MIN_LARGEST_FREE_BLOCK = 8192

# 1 = send every scanned frame.
# 2 = scan at 60Hz, send at 30Hz.
# 3 = scan at 60Hz, send at 20Hz.
SEND_EVERY_N_FRAMES = 1

ENABLE_IMU = True
ENABLE_BATTERY = True
ENABLE_LED = True
ENABLE_OTA = False

WIFI_MODE = "STA"
BOOT_WINDOW_MS = 3000
BOOT_WINDOW_POLL_MS = 50
DEVICE_STATE_DIR = "device_state"
RECOVERY_DIR = "recovery"
OS_DIR = "nhos"
OTA_STAGE_DIR = "ota_stage"
CALIBRATION_DIR = DEVICE_STATE_DIR + "/calibration"
DATA_FILES_DIR = "data/files"
DATA_LOG_DIR = "data/logs"
DATA_TMP_DIR = "data/tmp"
LOG_PATH = DATA_LOG_DIR + "/device.log"

USE_MATRIX_MOCK = False

MAGIC = 0xA55A
PACKET_VERSION = 1

# Matrix scanner
COL_ACTIVE_LEVEL = 1
COL_INACTIVE_LEVEL = 0
MATRIX_SETTLE_US = 20

ADC_ATTEN_11DB = True

# BQ25180 charger
BQ25180_ADDR = 0x6A
BQ25180_CHARGE_CURRENT_MA = 250
BQ25180_CHARGE_VOLTAGE_MV = 4200

# BMI270
BMI270_ADDR = 0x68
BMI270_FALLBACK_ADDR = 0x69

# Text output units:
# AX/AY/AZ: m/s^2
# GX/GY/GZ: deg/s
# CHIP_TEMP: degC or NA
BMI270_ACC_DECIMALS = 3
BMI270_GYRO_DECIMALS = 3
BMI270_TEMP_DECIMALS = 2

# LED behavior
CHARGING_LED_ENABLE = True

PRINT_FPS = False
PRINT_WIFI_STATUS = False
PRINT_MATRIX_INIT_DETAILS = False
PRINT_PACKET_ERROR = True
PRINT_PIN_CONFLICTS = True

GC_EVERY_N_FRAMES = 30
