import unittest
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[1]
BUILD_SCRIPT = REPO_ROOT / "firmware" / "scripts" / "build_firmware.sh"
FLASH_SCRIPT = REPO_ROOT / "firmware" / "scripts" / "flash_firmware.sh"
BOARD_DIR = REPO_ROOT / "firmware" / "micropython" / "boards" / "NEWHORIZONS_ESP32S3_N8"
MICROPYTHON_BIN = REPO_ROOT / "firmware" / "build" / "esp32s3" / "micropython.bin"


class FirmwareBuildConfigTests(unittest.TestCase):
    def test_build_script_uses_newhorizons_s3_n8_board(self):
        source = BUILD_SCRIPT.read_text(encoding="utf-8")

        self.assertIn("NEWHORIZONS_ESP32S3_N8", source)
        self.assertIn("BOARD_DIR=", source)
        self.assertIn('rm -f "${BUILD}/sdkconfig"', source)
        self.assertNotIn("BOARD=ESP32_GENERIC_S3", source)

    def test_board_config_is_8mb_flash_without_psram(self):
        cmake = (BOARD_DIR / "mpconfigboard.cmake").read_text(encoding="utf-8")
        sdkconfig = (BOARD_DIR / "sdkconfig.board").read_text(encoding="utf-8")

        self.assertIn("CONFIG_ESPTOOLPY_FLASHSIZE_8MB=y", sdkconfig)
        self.assertIn('CONFIG_ESPTOOLPY_FLASHSIZE="8MB"', sdkconfig)
        self.assertIn('CONFIG_PARTITION_TABLE_CUSTOM_FILENAME="partitions-newhorizons-8MiBplus.csv"', sdkconfig)
        source = (BOARD_DIR / "partitions-8MiBplus.csv").read_text(encoding="utf-8")
        self.assertIn("8 MB flash, stable 0x200000 VFS start", source)
        self.assertIn("factory,  app,  factory, 0x10000, 0x1F0000", source)
        self.assertNotIn("sdkconfig.spiram", cmake)
        self.assertNotIn("CONFIG_SPIRAM=y", sdkconfig)
        self.assertNotIn("CONFIG_SPIRAM_BOOT_INIT=y", sdkconfig)
        self.assertIn("CONFIG_ESP_WIFI_STATIC_RX_BUFFER_NUM=6", sdkconfig)
        self.assertIn("CONFIG_ESP_WIFI_DYNAMIC_RX_BUFFER_NUM=24", sdkconfig)
        self.assertIn("CONFIG_ESP_WIFI_DYNAMIC_TX_BUFFER_NUM=24", sdkconfig)
        self.assertIn("CONFIG_ESP_WIFI_MGMT_SBUF_NUM=32", sdkconfig)
        self.assertIn("CONFIG_ESP_WIFI_RX_BA_WIN=4", sdkconfig)

    def test_committed_micropython_image_declares_8mb_flash(self):
        header = MICROPYTHON_BIN.read_bytes()[:4]

        self.assertEqual(header[0], 0xE9)
        self.assertEqual(header[3] & 0xF0, 0x30)

    def test_flash_script_writes_as_8mb_flash(self):
        source = FLASH_SCRIPT.read_text(encoding="utf-8")

        self.assertIn("--flash_size 8MB", source)


if __name__ == "__main__":
    unittest.main()
