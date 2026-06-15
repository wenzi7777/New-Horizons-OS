#pragma once

#if defined(NHOS_BOARD_GCU_V21_LTS)

#define NHOS_BOARD_NAME         "VD-CTL/R v2.1 GCU LTS"
#define NHOS_BOARD_ROWS         10
#define NHOS_BOARD_COLS         12
#define NHOS_BOARD_I2C_HZ       1000000
#define NHOS_BOARD_BQ25180_I2C_HZ 400000
#define NHOS_BOARD_HAS_MAG      1
#define NHOS_BOARD_HAS_BQ25180  0
#define NHOS_BOARD_HAS_BUTTON   0
#define NHOS_BOARD_HAS_EXT_LED  0
#define NHOS_BOARD_HAS_OLED     0
#define NHOS_BOARD_SUPPORTS_GPIO_WAKE 0
#define NHOS_BOARD_DEFAULT_OTA_MANIFEST_URL \
  "https://raw.githubusercontent.com/wenzi7777/New-Horizons-OS/main/releases/arduino-gcu-v21-lts-latest.json"

#elif defined(NHOS_BOARD_GCU_V22C_LTS)

#define NHOS_BOARD_NAME         "VD-CTL/R v2.2.C GCU LTS"
#define NHOS_BOARD_ROWS         11
#define NHOS_BOARD_COLS         13
#define NHOS_BOARD_I2C_HZ       1000000
#define NHOS_BOARD_BQ25180_I2C_HZ 400000
#define NHOS_BOARD_HAS_MAG      1
#define NHOS_BOARD_HAS_BQ25180  0
#define NHOS_BOARD_HAS_BUTTON   0
#define NHOS_BOARD_HAS_EXT_LED  0
#define NHOS_BOARD_HAS_OLED     0
#define NHOS_BOARD_SUPPORTS_GPIO_WAKE 0
#define NHOS_BOARD_DEFAULT_OTA_MANIFEST_URL \
  "https://raw.githubusercontent.com/wenzi7777/New-Horizons-OS/main/releases/arduino-gcu-v22c-lts-latest.json"

#elif defined(NHOS_BOARD_GCU_V23D_LTS)

#define NHOS_BOARD_NAME         "VD-CTL/R v2.3.D GCU LTS"
#define NHOS_BOARD_ROWS         15
#define NHOS_BOARD_COLS         15
#define NHOS_BOARD_I2C_HZ       1000000
#define NHOS_BOARD_BQ25180_I2C_HZ 400000
#define NHOS_BOARD_HAS_MAG      1
#define NHOS_BOARD_HAS_BQ25180  1
#define NHOS_BOARD_HAS_BUTTON   0
#define NHOS_BOARD_HAS_EXT_LED  0
#define NHOS_BOARD_HAS_OLED     0
#define NHOS_BOARD_SUPPORTS_GPIO_WAKE 0
#define NHOS_BOARD_DEFAULT_OTA_MANIFEST_URL \
  "https://raw.githubusercontent.com/wenzi7777/New-Horizons-OS/main/releases/arduino-gcu-lts-latest.json"

#else

#define NHOS_BOARD_NAME         "VD-CTL/R v1.0.F 2026.4"
#define NHOS_BOARD_ROWS         10
#define NHOS_BOARD_COLS         21
#define NHOS_BOARD_I2C_HZ       400000
#define NHOS_BOARD_BQ25180_I2C_HZ 400000
#define NHOS_BOARD_HAS_MAG      0
#define NHOS_BOARD_HAS_BQ25180  1
#define NHOS_BOARD_HAS_BUTTON   1
#define NHOS_BOARD_HAS_EXT_LED  1
#define NHOS_BOARD_HAS_OLED     1
#define NHOS_BOARD_SUPPORTS_GPIO_WAKE 1
#define NHOS_BOARD_DEFAULT_OTA_MANIFEST_URL \
  "https://raw.githubusercontent.com/wenzi7777/New-Horizons-OS/main/releases/arduino-latest.json"

#endif
