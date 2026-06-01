import re
import unittest
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[1]
ARDUINO_ROOT = REPO_ROOT / "firmware" / "newhorizons_os"
SCRIPT_ROOT = REPO_ROOT / "firmware" / "scripts"


class ArduinoRewriteScaffoldTests(unittest.TestCase):
    def test_repository_uses_hardware_firmware_layout(self):
        readme = (REPO_ROOT / "README.md").read_text(encoding="utf-8")
        gitignore = (REPO_ROOT / ".gitignore").read_text(encoding="utf-8")

        self.assertTrue(ARDUINO_ROOT.is_dir())
        self.assertTrue(SCRIPT_ROOT.is_dir())
        self.assertIn("firmware/newhorizons_os/", readme)
        self.assertIn("firmware/scripts/", readme)
        self.assertIn("hardware/vd-ctl-r-v1.0f/", readme)
        self.assertIn("firmware/build/", gitignore)
        self.assertNotIn("firmware/arduino/newhorizons_os", readme)

    def test_required_arduino_modules_exist(self):
        expected = {
            "newhorizons_os.ino",
            "Config.h",
            "Config.cpp",
            "BoardPins.h",
            "BoardPins.cpp",
            "LedController.h",
            "LedController.cpp",
            "DeviceConfig.h",
            "DeviceConfig.cpp",
            "DisplayManager.h",
            "DisplayManager.cpp",
            "ExternalLedController.h",
            "ExternalLedController.cpp",
            "PowerManager.h",
            "PowerManager.cpp",
            "BootModeManager.h",
            "BootModeManager.cpp",
            "MatrixScanner.h",
            "MatrixScanner.cpp",
            "PacketBuilder.h",
            "PacketBuilder.cpp",
            "Calibration.h",
            "Calibration.cpp",
            "FindMeClient.h",
            "FindMeClient.cpp",
            "WifiManager.h",
            "WifiManager.cpp",
            "ControlServer.h",
            "ControlServer.cpp",
            "OtaManager.h",
            "OtaManager.cpp",
            "Storage.h",
            "Storage.cpp",
        }

        missing = sorted(name for name in expected if not (ARDUINO_ROOT / name).exists())

        self.assertEqual(missing, [])

    def test_config_declares_new_protocol_packet_version_and_ports(self):
        config = (ARDUINO_ROOT / "Config.h").read_text(encoding="utf-8")

        self.assertIn('kProtocolName[] = "NHO/Arduino/1"', config)
        self.assertIn("kPacketVersion = 3", config)
        self.assertIn("kUdpStreamPort = 13250", config)
        self.assertIn("kDiscoveryPort = 22346", config)
        self.assertIn("kControlPort = 22345", config)
        self.assertIn('kHardwareModel[] = "VD-CTL/R v1.0.F 2026.4"', config)
        self.assertIn('kFirmwareVersion[] = "v0.5.15"', config)
        self.assertNotIn('kFirmwareVersion[] = "v0.5.0-arduino"', config)

    def test_wifi_setup_ap_uses_legacy_open_ssid(self):
        config = (ARDUINO_ROOT / "Config.h").read_text(encoding="utf-8")

        self.assertIn('kDefaultApSsidPrefix[] = "NewHorizonsOS"', config)
        self.assertNotIn('kDefaultApSsidPrefix[] = "NewHorizonsOS-Arduino"', config)
        self.assertIn('kDefaultApPassword[] = ""', config)
        self.assertIn('kSetupPortalDomain[] = "newhorizons.os"', config)
        self.assertIn("kSetupPortalPort = 80", config)

    def test_findme_is_primary_gateway_attachment_flow(self):
        header = (ARDUINO_ROOT / "FindMeClient.h").read_text(encoding="utf-8")
        findme = (ARDUINO_ROOT / "FindMeClient.cpp").read_text(encoding="utf-8")
        sketch = (ARDUINO_ROOT / "newhorizons_os.ino").read_text(encoding="utf-8")
        control = (ARDUINO_ROOT / "ControlServer.cpp").read_text(encoding="utf-8")

        self.assertIn("class FindMeClient", header)
        self.assertIn("findme_discover", findme)
        self.assertIn("findme_offer", findme)
        self.assertIn("preferred_gateway_id", findme)
        self.assertIn("claim_id", findme)
        self.assertIn("kDiscoveryPort", findme)
        self.assertIn("findme.begin", sketch)
        self.assertIn("findme.service", sketch)
        self.assertIn("findme.streamHost", sketch)
        self.assertNotIn('host = "255.255.255.255"', sketch)
        self.assertIn('cmd == "findme_discover"', control)
        self.assertIn('cmd == "findme_switch_gateway"', control)
        self.assertIn('data += ",\\"findme\\":";', control)
        self.assertIn("findme_->statusJson()", control)

    def test_arduino_file_management_uses_read_write_command_contract(self):
        control = (ARDUINO_ROOT / "ControlServer.cpp").read_text(encoding="utf-8")

        for command in (
            'cmd == "storage_status"',
            'cmd == "file_read_begin"',
            'cmd == "file_read_chunk"',
            'cmd == "file_write_begin"',
            'cmd == "file_write_chunk"',
            'cmd == "file_write_finish"',
            'cmd == "file_delete"',
        ):
            self.assertIn(command, control)
        self.assertIn("maintenance_required", control)

    def test_manual_calibration_workbench_command_set_exists(self):
        control = (ARDUINO_ROOT / "ControlServer.cpp").read_text(encoding="utf-8")
        calibration_header = (ARDUINO_ROOT / "Calibration.h").read_text(encoding="utf-8")
        calibration_impl = (ARDUINO_ROOT / "Calibration.cpp").read_text(encoding="utf-8")
        scanner_header = (ARDUINO_ROOT / "MatrixScanner.h").read_text(encoding="utf-8")
        scanner_impl = (ARDUINO_ROOT / "MatrixScanner.cpp").read_text(encoding="utf-8")
        sketch = (ARDUINO_ROOT / "newhorizons_os.ino").read_text(encoding="utf-8")

        for command in (
            'cmd == "calibration_status"',
            'cmd == "calibration_enable"',
            'cmd == "calibration_disable"',
            'cmd == "calibration_clear_profile"',
            'cmd == "calibration_dump_level"',
            'cmd == "calibration_delete_level"',
            'cmd == "calibration_session_begin"',
            'cmd == "calibration_session_abort"',
            'cmd == "calibration_session_commit"',
            'cmd == "calibration_capture_cell"',
            'cmd == "calibration_capture_all"',
        ):
            self.assertIn(command, control)
        self.assertIn('data += ",\\"calibration\\":";', control)
        self.assertIn("calibration_ ? calibration_->statusJson", control)
        self.assertIn("bool sessionBegin();", calibration_header)
        self.assertIn("bool captureCell", calibration_header)
        self.assertIn("bool captureAll", calibration_header)
        self.assertIn("bool dumpLevelJson", calibration_header)
        self.assertIn("bool deleteLevel", calibration_header)
        self.assertIn("bool apply(float rawMv, uint16_t sensorIndex, float& outValue) const;", calibration_header)
        self.assertIn('"draft_levels"', calibration_impl)
        self.assertIn('"metadata"', calibration_impl)
        self.assertIn("void setCalibration", scanner_header)
        self.assertIn("bool captureCellAverage", scanner_header)
        self.assertIn("bool captureAllAverages", scanner_header)
        self.assertIn("calibration_->apply", scanner_impl)
        self.assertIn("scanner.setCalibration(&calibration)", sketch)

    def test_diagnostics_commands_report_heap_totals_and_storage_usage(self):
        control = (ARDUINO_ROOT / "ControlServer.cpp").read_text(encoding="utf-8")
        storage_header = (ARDUINO_ROOT / "Storage.h").read_text(encoding="utf-8")
        storage = (ARDUINO_ROOT / "Storage.cpp").read_text(encoding="utf-8")

        self.assertIn("ESP.getHeapSize()", control)
        self.assertIn('"heap_total"', control)
        self.assertIn('"heap_used"', control)
        self.assertIn("String storageStatusJson()", storage_header)
        self.assertIn("SPIFFS.totalBytes()", storage)
        self.assertIn("SPIFFS.usedBytes()", storage)
        self.assertIn('"categories"', storage)

    def test_wifi_setup_ssid_matches_legacy_full_device_suffix(self):
        wifi = (ARDUINO_ROOT / "WifiManager.cpp").read_text(encoding="utf-8")

        self.assertRegex(
            wifi,
            re.compile(
                r'snprintf\(\s*ssid,\s*sizeof\(ssid\),\s*"%s-%02X%02X%02X%02X%02X%02X"',
                re.S,
            ),
        )
        self.assertIn("WiFi.softAP(ssid);", wifi)
        self.assertNotIn("WiFi.softAP(ssid, kDefaultApPassword);", wifi)

    def test_wifi_setup_starts_captive_portal_dns_and_http(self):
        header = (ARDUINO_ROOT / "WifiManager.h").read_text(encoding="utf-8")
        wifi = (ARDUINO_ROOT / "WifiManager.cpp").read_text(encoding="utf-8")

        self.assertIn("#include <DNSServer.h>", header)
        self.assertIn("#include <WebServer.h>", header)
        self.assertIn("DNSServer dnsServer_", header)
        self.assertIn("WebServer portalServer_", header)
        self.assertIn("dnsServer_.start", wifi)
        self.assertIn("portalServer_.begin()", wifi)
        self.assertIn("portalServer_.handleClient()", wifi)
        self.assertIn("enableDhcpCaptivePortal", wifi)
        self.assertIn("handlePortalSave", wifi)

    def test_wifi_setup_enters_portal_without_credentials_or_boot_button_request(self):
        header = (ARDUINO_ROOT / "WifiManager.h").read_text(encoding="utf-8")
        wifi = (ARDUINO_ROOT / "WifiManager.cpp").read_text(encoding="utf-8")
        sketch = (ARDUINO_ROOT / "newhorizons_os.ino").read_text(encoding="utf-8")

        self.assertIn("bool begin(Storage& storage, bool forceSetupPortal = false);", header)
        self.assertIn("bool hasCredentials() const;", header)
        self.assertIn("if (!hasCredentials())", wifi)
        self.assertIn("wifi_sta_no_credentials", wifi)
        self.assertIn("if (forceSetupPortal)", wifi)
        self.assertIn("wifi_setup_requested_by_action_button", wifi)
        self.assertIn("wifi.begin(storage, bootMode.wifiSetupRequested())", sketch)

    def test_force_setup_portal_does_not_prestart_sta_or_disconnect_missing_ap(self):
        wifi = (ARDUINO_ROOT / "WifiManager.cpp").read_text(encoding="utf-8")

        self.assertLess(wifi.index("if (forceSetupPortal)"), wifi.index("WiFi.mode(WIFI_STA)"))
        self.assertIn("#include <esp_mac.h>", wifi)
        self.assertIn("esp_read_mac(mac, ESP_MAC_WIFI_STA)", wifi)
        self.assertIn("const bool apWasActive = portalStarted_ || setupActive_;", wifi)
        self.assertRegex(
            wifi,
            re.compile(r"if\s*\(apWasActive\)\s*\{\s*WiFi\.softAPdisconnect\(true\);\s*\}", re.S),
        )

    def test_action_button_boot_window_requests_wifi_setup(self):
        config = (ARDUINO_ROOT / "Config.h").read_text(encoding="utf-8")
        header = (ARDUINO_ROOT / "BootModeManager.h").read_text(encoding="utf-8")
        boot = (ARDUINO_ROOT / "BootModeManager.cpp").read_text(encoding="utf-8")

        self.assertIn("kBootWifiSetupWindowMs = 3000", config)
        self.assertIn("bool wifiSetupRequested() const;", header)
        self.assertIn("wifiSetupRequested_", header)
        self.assertIn("sampleWifiSetupButtonWindow", boot)
        self.assertIn("millis() - started < kBootWifiSetupWindowMs", boot)
        self.assertIn("digitalRead(kActionButtonPin) == LOW", boot)
        self.assertIn("boot_action_button_setup_requested", boot)
        self.assertNotIn("actionButtonHeld() ||", boot)

    def test_wifi_setup_portal_lists_scanned_networks(self):
        header = (ARDUINO_ROOT / "WifiManager.h").read_text(encoding="utf-8")
        wifi = (ARDUINO_ROOT / "WifiManager.cpp").read_text(encoding="utf-8")

        self.assertIn("String wifiNetworkOptionsHtml() const;", header)
        self.assertIn("WiFi.scanNetworks", wifi)
        self.assertIn('id=\\"ssid_select\\"', wifi)
        self.assertIn('onchange=\\"document.getElementById(\\\'ssid\\\').value=this.value\\"', wifi)
        self.assertIn("WiFi.RSSI(i)", wifi)
        self.assertIn("WiFi.encryptionType(i)", wifi)

    def test_serial_boot_log_has_operational_milestones(self):
        sketch = (ARDUINO_ROOT / "newhorizons_os.ino").read_text(encoding="utf-8")
        wifi = (ARDUINO_ROOT / "WifiManager.cpp").read_text(encoding="utf-8")
        control = (ARDUINO_ROOT / "ControlServer.cpp").read_text(encoding="utf-8")

        for marker in (
            "boot_stage=storage_ready",
            "boot_stage=boot_mode_ready",
            "boot_stage=i2c_ready",
            "boot_stage=scanner_ready",
            "boot_stage=wifi_ready",
            "boot_stage=ota_ready",
            "boot_stage=control_ready",
            "runtime_ready protocol=",
        ):
            self.assertIn(marker, sketch)
        self.assertIn("wifi_setup_ap_started ssid=", wifi)
        self.assertIn("wifi_sta_connected ip=", wifi)
        self.assertIn("control_server_started port=", control)

    def test_sk6812_status_patterns_include_bq25180_background_states(self):
        header = (ARDUINO_ROOT / "LedController.h").read_text(encoding="utf-8")
        led = (ARDUINO_ROOT / "LedController.cpp").read_text(encoding="utf-8")
        control = (ARDUINO_ROOT / "ControlServer.cpp").read_text(encoding="utf-8")
        self.assertTrue((ARDUINO_ROOT / "PowerManager.h").exists())
        self.assertTrue((ARDUINO_ROOT / "PowerManager.cpp").exists())
        power_header = (ARDUINO_ROOT / "PowerManager.h").read_text(encoding="utf-8")
        power = (ARDUINO_ROOT / "PowerManager.cpp").read_text(encoding="utf-8")
        sketch = (ARDUINO_ROOT / "newhorizons_os.ino").read_text(encoding="utf-8")

        self.assertIn("enum class LedSignal", header)
        self.assertIn("ChargeDone", header)
        self.assertIn("ChargingOrMissing", header)
        self.assertIn("0x39, 0xc5, 0xbb", header + led)
        self.assertRegex(led, re.compile(r"ChargeDone.*?5000.*?1", re.S))
        self.assertRegex(led, re.compile(r"ChargingOrMissing.*?10000.*?2", re.S))
        self.assertRegex(led, re.compile(r"Online.*?LedPalette::Off", re.S))
        self.assertIn("showEvent(LedSignal::CommandReceived)", control)
        self.assertIn("showEvent(responseOk ? LedSignal::CommandSuccess : LedSignal::CommandFailed)", control)
        self.assertIn("enum class ChargeState", power_header)
        self.assertIn("kBq25180Address = 0x6A", power)
        self.assertIn("kBq25180Stat0Register = 0x00", power)
        self.assertIn("(stat0 >> 5) & 0x03", power)
        self.assertIn("ChargeState::ChargeDone", power)
        self.assertIn("power.service(millis())", sketch)
        self.assertIn("power.statusJson()", sketch)
        self.assertIn('data += ",\\"battery\\":";', control)
        self.assertIn("power_->statusJson()", control)

    def test_bq25180_charge_profiles_include_safe_and_fast_modes(self):
        power_header = (ARDUINO_ROOT / "PowerManager.h").read_text(encoding="utf-8")
        power = (ARDUINO_ROOT / "PowerManager.cpp").read_text(encoding="utf-8")
        control = (ARDUINO_ROOT / "ControlServer.cpp").read_text(encoding="utf-8")
        sketch = (ARDUINO_ROOT / "newhorizons_os.ino").read_text(encoding="utf-8")

        self.assertIn("enum class ChargeProfile", power_header)
        self.assertIn("Compatible", power_header)
        self.assertIn("Fast", power_header)
        self.assertIn("applyProfileByName", power_header)
        self.assertIn("kBq25180VbatCtrlRegister = 0x03", power)
        self.assertIn("kBq25180IchgCtrlRegister = 0x04", power)
        self.assertIn("kBq25180ChargeCtrl0Register = 0x05", power)
        self.assertIn("kBq25180IcCtrlRegister = 0x07", power)
        self.assertIn("kBq25180TmrIlimRegister = 0x08", power)
        self.assertIn('"compatible"', power)
        self.assertIn('"fast"', power)
        self.assertIn("250, 500, 0x34, 0x05", power)
        self.assertIn("300, 500, 0x39, 0x05", power)
        self.assertNotIn("safe_default", power)
        self.assertNotIn("fast_800mah_only", power)
        self.assertIn("updateRegister(kBq25180ChargeCtrl0Register, 0x70, 0x20)", power)
        self.assertIn("updateRegister(kBq25180IcCtrlRegister, 0x0C, 0x04)", power)
        self.assertIn('\\"charge_current_ma\\"', power)
        self.assertIn('\\"input_limit_ma\\"', power)
        self.assertIn('\\"configured\\"', power)
        self.assertIn('cmd == "set_charge_profile"', control)
        self.assertIn('storage_->putString("charge_profile"', control)

    def test_device_config_persists_webui_runtime_settings_to_spiffs(self):
        header = (ARDUINO_ROOT / "DeviceConfig.h").read_text(encoding="utf-8")
        config = (ARDUINO_ROOT / "DeviceConfig.cpp").read_text(encoding="utf-8")
        storage_header = (ARDUINO_ROOT / "Storage.h").read_text(encoding="utf-8")
        storage = (ARDUINO_ROOT / "Storage.cpp").read_text(encoding="utf-8")
        control = (ARDUINO_ROOT / "ControlServer.cpp").read_text(encoding="utf-8")
        sketch = (ARDUINO_ROOT / "newhorizons_os.ino").read_text(encoding="utf-8")

        self.assertIn("struct DeviceConfigData", header)
        self.assertIn('kDeviceConfigPath[] = "/config/device.json"', config)
        self.assertIn("readTextFile", storage_header)
        self.assertIn("writeTextFileAtomic", storage_header)
        self.assertIn('SPIFFS.mkdir("/config")', storage)
        self.assertIn("SPIFFS.rename", storage)
        for key in (
            '"schema_version"',
            '"matrix_layout"',
            '"configured"',
            '"scan_timing"',
            '"filter"',
            '"imu"',
            '"logging"',
            '"ota"',
            '"indicators"',
            '"external_led"',
            '"oled"',
        ):
            self.assertIn(key, config)
        self.assertIn("deviceConfig.load(storage)", sketch)
        self.assertIn("deviceConfig_->save", control)
        self.assertIn("deviceConfig_->statusJson()", control)
        self.assertNotIn("manual_preset", config + control)

    def test_missing_device_config_leaves_matrix_layout_empty_and_scan_stopped(self):
        config = (ARDUINO_ROOT / "DeviceConfig.cpp").read_text(encoding="utf-8")
        scanner_header = (ARDUINO_ROOT / "MatrixScanner.h").read_text(encoding="utf-8")
        scanner = (ARDUINO_ROOT / "MatrixScanner.cpp").read_text(encoding="utf-8")
        sketch = (ARDUINO_ROOT / "newhorizons_os.ino").read_text(encoding="utf-8")

        defaults_body = re.search(r"void DeviceConfig::setDefaults\(\) \{(?P<body>.*?)\n\}", config, re.S)
        self.assertIsNotNone(defaults_body)
        self.assertIn("data_.schemaVersion = 2", defaults_body.group("body"))
        self.assertIn("data_.matrixLayout.analogCount = 0", defaults_body.group("body"))
        self.assertIn("data_.matrixLayout.selectCount = 0", defaults_body.group("body"))
        self.assertNotIn("kRowAdcPins", defaults_body.group("body"))
        self.assertNotIn("kColPins", defaults_body.group("body"))
        self.assertIn('extractBool(matrix, "configured", false)', config)
        self.assertIn("bool hasLayout() const;", scanner_header)
        self.assertIn("size_t rowCount_ = 0", scanner_header)
        self.assertIn("size_t colCount_ = 0", scanner_header)
        self.assertNotIn("memcpy(rows_, kRowAdcPins", scanner)
        self.assertNotIn("rowCount_ = kRowAdcPinCount", scanner)
        self.assertIn("scanner.hasLayout()", sketch)
        self.assertIn("scan_task_deferred matrix_layout_empty", sketch)
        self.assertIn("scanner.matrixShapeJson()", sketch)

    def test_legacy_schema_matrix_layout_is_migrated_when_arrays_exist(self):
        config = (ARDUINO_ROOT / "DeviceConfig.cpp").read_text(encoding="utf-8")

        self.assertIn('const bool configured = extractBool(matrix, "configured", false) || (storedSchemaVersion < 2 && analogCount && selectCount);', config)
        self.assertNotIn('const bool configured = storedSchemaVersion >= 2 && extractBool(matrix, "configured", false);', config)

    def test_set_matrix_layout_starts_deferred_scanner_without_reboot(self):
        control = (ARDUINO_ROOT / "ControlServer.cpp").read_text(encoding="utf-8")

        self.assertIn('cmd == "set_matrix_layout"', control)
        self.assertIn("boot_->mode() == RunMode::Normal", control)
        self.assertIn("scanner_->hasLayout()", control)
        self.assertIn("!scanner_->active()", control)
        self.assertIn("scanner_->start()", control)
        self.assertIn("scan_task_started_by_layout_update", control)

    def test_arduino_runtime_initializes_bmi270_and_streams_imu_payload(self):
        sketch = (ARDUINO_ROOT / "newhorizons_os.ino").read_text(encoding="utf-8")
        control_header = (ARDUINO_ROOT / "ControlServer.h").read_text(encoding="utf-8")
        control = (ARDUINO_ROOT / "ControlServer.cpp").read_text(encoding="utf-8")
        imu_header = (ARDUINO_ROOT / "ImuManager.h").read_text(encoding="utf-8")
        imu_impl = (ARDUINO_ROOT / "ImuManager.cpp").read_text(encoding="utf-8")
        packet_header = (ARDUINO_ROOT / "PacketBuilder.h").read_text(encoding="utf-8")
        packet_impl = (ARDUINO_ROOT / "PacketBuilder.cpp").read_text(encoding="utf-8")

        self.assertIn('#include "Arduino_BMI270_BMM150.h"', imu_header)
        self.assertIn("IMU.begin(BOSCH_ACCELEROMETER_ONLY)", imu_impl)
        self.assertNotIn("readMagneticField", imu_impl)
        self.assertIn("boot_stage=imu_ready", sketch)
        self.assertIn("imu.service", sketch)
        self.assertNotIn("imu.readSample", sketch)
        self.assertIn("bool copyLatestSample(float out7[7]) const;", imu_header)
        self.assertIn("void service(uint32_t nowUs);", imu_header)
        self.assertIn("IMU.setContinuousMode()", imu_impl)
        self.assertIn("lastReadDurationUs_", imu_header)
        self.assertIn("sampleRateHz_", imu_header)
        self.assertIn("cacheAgeMs", imu_impl)
        self.assertIn("buildMatrixPacketHeader(frame, packetBuffer, sizeof(packetBuffer), matrixPayloadLen, imuSampleValid ? imuSample : nullptr)", sketch)
        self.assertIn("const float* imu7", packet_header)
        self.assertIn("out[3] = imu7 ? kPacketFlagImu : 0", packet_impl)
        self.assertIn("putFloat(out + offset, imu7[i])", packet_impl)
        self.assertIn("ImuManager& imu", control_header)
        self.assertIn("imu_ ? imu_->statusJson()", control)

    def test_log_configuration_defaults_to_rolling_16k_and_has_extended_32k_mode(self):
        config = (ARDUINO_ROOT / "Config.h").read_text(encoding="utf-8")
        device_header = (ARDUINO_ROOT / "DeviceConfig.h").read_text(encoding="utf-8")
        device_config = (ARDUINO_ROOT / "DeviceConfig.cpp").read_text(encoding="utf-8")
        storage_header = (ARDUINO_ROOT / "Storage.h").read_text(encoding="utf-8")
        storage = (ARDUINO_ROOT / "Storage.cpp").read_text(encoding="utf-8")
        control = (ARDUINO_ROOT / "ControlServer.cpp").read_text(encoding="utf-8")
        sketch = (ARDUINO_ROOT / "newhorizons_os.ino").read_text(encoding="utf-8")

        self.assertIn("kDefaultLogMaxBytes = 16 * 1024", config)
        self.assertIn("kExtendedLogMaxBytes = 32 * 1024", config)
        self.assertIn("struct LogConfig", device_header)
        self.assertIn("LogConfig logging", device_header)
        self.assertIn('data_.logging.enabled = true', device_config)
        self.assertIn('data_.logging.maxBytes = kDefaultLogMaxBytes', device_config)
        self.assertIn('data_.logging.level = "info"', device_config)
        self.assertIn('data_.logging.mode = "standard"', device_config)
        self.assertIn("configureLog", storage_header)
        self.assertIn("logStatusJson", storage_header)
        self.assertIn("logMaxBytes_", storage)
        self.assertIn("rotateLogIfNeeded", storage)
        self.assertIn('cmd == "set_log"', control)
        self.assertIn("storage_->configureLog", control)
        self.assertIn('data += ",\\"logging\\":";', control)
        self.assertIn("storage.configureLog", sketch)

    def test_boot_auto_ota_is_configurable_and_runs_after_wifi_ready(self):
        device_header = (ARDUINO_ROOT / "DeviceConfig.h").read_text(encoding="utf-8")
        device_config = (ARDUINO_ROOT / "DeviceConfig.cpp").read_text(encoding="utf-8")
        ota_header = (ARDUINO_ROOT / "OtaManager.h").read_text(encoding="utf-8")
        ota = (ARDUINO_ROOT / "OtaManager.cpp").read_text(encoding="utf-8")
        sketch = (ARDUINO_ROOT / "newhorizons_os.ino").read_text(encoding="utf-8")
        control = (ARDUINO_ROOT / "ControlServer.cpp").read_text(encoding="utf-8")

        self.assertIn("struct OtaConfig", device_header)
        self.assertIn("OtaConfig ota", device_header)
        self.assertIn("autoApplyOnBoot", device_header)
        self.assertIn("data_.ota.autoApplyOnBoot = false", device_config)
        self.assertIn('"auto_apply_on_boot"', device_config)
        self.assertIn("bool autoApplyIfNewer", ota_header)
        self.assertIn("auto_ota_check_start", ota)
        self.assertIn("auto_ota_no_update", ota)
        self.assertIn("serviceAutoOta", sketch)
        self.assertIn("deviceConfig.data().ota.autoApplyOnBoot", sketch)
        self.assertIn('cmd == "set_ota_config"', control)
        self.assertIn("LedSignal::OtaActive", sketch)
        self.assertIn("LedSignal::OtaSuccess", sketch)
        self.assertIn("LedSignal::OtaError", sketch)
        self.assertIn("auto_ota_apply_failed", sketch)
        self.assertIn("firmware_download_timeout", ota)
        self.assertIn("update_started", control)
        self.assertIn("servicePendingApplyUpdate", control)

    def test_release_scripts_use_stock_8mb_dual_ota_partition(self):
        build_script = (SCRIPT_ROOT / "build_arduino_release.sh").read_text(encoding="utf-8")
        flash_script = (SCRIPT_ROOT / "flash_arduino_firmware.sh").read_text(encoding="utf-8")

        self.assertFalse((ARDUINO_ROOT / "partitions.csv").exists())
        self.assertIn("PartitionScheme=default_8MB", build_script)
        self.assertIn("PartitionScheme=default_8MB", flash_script)
        self.assertNotIn("PartitionScheme=custom", build_script)
        self.assertNotIn("PartitionScheme=custom", flash_script)

    def test_ssd1306_128x32_driver_has_off_auto_enabled_modes(self):
        header = (ARDUINO_ROOT / "DisplayManager.h").read_text(encoding="utf-8")
        display = (ARDUINO_ROOT / "DisplayManager.cpp").read_text(encoding="utf-8")
        control = (ARDUINO_ROOT / "ControlServer.cpp").read_text(encoding="utf-8")
        sketch = (ARDUINO_ROOT / "newhorizons_os.ino").read_text(encoding="utf-8")

        self.assertIn("#include <Adafruit_SSD1306.h>", header)
        self.assertIn("kOledWidth = 128", display)
        self.assertIn("kOledHeight = 32", display)
        self.assertIn("enum class OledMode", header)
        self.assertIn("Auto", header)
        self.assertIn("Enabled", header)
        self.assertIn("Off", header)
        self.assertIn("0x3C", display)
        self.assertIn("0x3D", display)
        self.assertIn("probeAddress", display)
        self.assertIn("SSD1306_SWITCHCAPVCC", display)
        self.assertIn('"off"', display + control)
        self.assertIn('"auto"', display + control)
        self.assertIn('"enabled"', display + control)
        self.assertIn("validOledMode", control)
        self.assertIn("displayManager.begin", sketch)
        self.assertIn("displayManager.service", sketch)
        self.assertIn("display_->statusJson()", control)

    def test_oled_pages_render_operational_labels_and_gateway_metrics(self):
        header = (ARDUINO_ROOT / "DisplayManager.h").read_text(encoding="utf-8")
        display = (ARDUINO_ROOT / "DisplayManager.cpp").read_text(encoding="utf-8")
        sketch = (ARDUINO_ROOT / "newhorizons_os.ino").read_text(encoding="utf-8")

        self.assertIn("gatewayIp", header)
        self.assertIn("heapTotal", header)
        self.assertIn("NHOS ", display)
        self.assertIn("kFirmwareVersion", display)
        self.assertIn("GW ", display)
        self.assertIn("RAM", display)
        self.assertIn("Sensor snapshot", display)
        self.assertIn("Grid ", display)
        self.assertIn("Scan ", display)
        self.assertIn("Over budget ", display)
        self.assertIn("packets", display)
        self.assertIn("UDP ", display)
        self.assertNotIn('display_.print("Last ")', display)
        self.assertNotIn('display_.print("Overrun ")', display)
        self.assertIn("findme.hasGateway()", sketch)
        self.assertIn("ESP.getHeapSize()", sketch)

    def test_external_ws2812b_controller_is_separate_from_internal_sk6812_status_led(self):
        status_header = (ARDUINO_ROOT / "LedController.h").read_text(encoding="utf-8")
        status_impl = (ARDUINO_ROOT / "LedController.cpp").read_text(encoding="utf-8")
        external_header = (ARDUINO_ROOT / "ExternalLedController.h").read_text(encoding="utf-8")
        external = (ARDUINO_ROOT / "ExternalLedController.cpp").read_text(encoding="utf-8")
        control = (ARDUINO_ROOT / "ControlServer.cpp").read_text(encoding="utf-8")
        sketch = (ARDUINO_ROOT / "newhorizons_os.ino").read_text(encoding="utf-8")

        self.assertIn("class LedController", status_header)
        self.assertIn("kStatusLedPin", status_impl)
        self.assertNotIn("kExternalLedPin", status_header + status_impl)
        self.assertNotIn("setExternal", status_header + status_impl)
        self.assertIn("class ExternalLedController", external_header)
        self.assertIn("#include <Adafruit_NeoPixel.h>", external_header)
        self.assertIn("Adafruit_NeoPixel pixels_", external_header)
        self.assertIn("kExternalLedCount", external)
        self.assertIn("kExternalLedPin", external)
        self.assertIn('"identify"', external + control)
        self.assertIn("identifyStartedMs_", external_header)
        self.assertIn("showSolid", external_header + external)
        self.assertIn("showSolid(LedPalette::FindMePending", external)
        self.assertIn('pin\\":', external)
        self.assertIn('initialized\\":', external)
        self.assertIn('last_show_ms\\":', external)
        self.assertIn('last_error\\":', external)
        self.assertIn('"off"', external + control)
        self.assertIn('"enabled"', external + control)
        self.assertIn("validExternalLedMode", control)
        self.assertNotIn("external_led", status_header)
        self.assertIn("externalLeds.begin", sketch)
        self.assertIn("externalLeds.service", sketch)
        self.assertIn("externalLeds_->statusJson()", control)
        self.assertIn("status_led", control)
        self.assertIn("external_led", control)
        self.assertIn('storage.getString("charge_profile", "compatible")', sketch)

    def test_control_server_float_parser_reads_numeric_brightness_without_falling_into_next_key(self):
        control = (ARDUINO_ROOT / "ControlServer.cpp").read_text(encoding="utf-8")
        start = control.index("float ControlServer::extractFloat")
        end = control.index("bool ControlServer::extractBool", start)
        body = control[start:end]

        self.assertNotIn("extractString(request, key)", body)
        self.assertIn('String value = request.substring(colon + 1, end);', body)
        self.assertIn("value.trim();", body)
        self.assertIn("return value.toFloat();", body)

    def test_board_pin_map_matches_new_horizons_hardware(self):
        pins = (ARDUINO_ROOT / "BoardPins.cpp").read_text(encoding="utf-8")

        self.assertRegex(pins, re.compile(r"kRowAdcPins\[\].*=\s*\{\s*1,\s*2,\s*3,\s*4,\s*5,\s*6,\s*7,\s*8,\s*9,\s*10\s*\}", re.S))
        self.assertRegex(pins, re.compile(r"kColPins\[\].*=\s*\{\s*13,\s*14,\s*15,\s*16,\s*17,\s*18,\s*19,\s*20,\s*21,\s*26,\s*47,\s*33,\s*34,\s*48,\s*35,\s*36,\s*37,\s*38,\s*39,\s*40,\s*41\s*\}", re.S))
        self.assertIn("kI2cScl = 42", pins)
        self.assertIn("kI2cSda = 45", pins)
        self.assertIn("kStatusLedPin = 11", pins)
        self.assertIn("kExternalLedPin = 12", pins)
        self.assertIn("kActionButtonPin = 46", pins)

    def test_release_scripts_exist(self):
        expected = {
            "build_arduino_release.sh",
            "flash_arduino_firmware.sh",
            "generate_arduino_manifest.py",
        }

        missing = sorted(name for name in expected if not (SCRIPT_ROOT / name).exists())

        self.assertEqual(missing, [])

    def test_release_script_publishes_trackable_artifacts(self):
        script = (SCRIPT_ROOT / "build_arduino_release.sh").read_text(encoding="utf-8")

        self.assertIn('RELEASE_DIR="${ROOT}/releases/artifacts"', script)
        self.assertIn('target="${RELEASE_DIR}/newhorizons-os-${VERSION}.bin"', script)
        self.assertIn('VERSION="${VERSION:-v0.5.15}"', script)
        self.assertNotIn('VERSION="${VERSION:-v0.5.0-arduino}"', script)


if __name__ == "__main__":
    unittest.main()
