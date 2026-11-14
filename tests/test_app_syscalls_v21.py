"""v2.1.0 app system calls: storage, messages, device control, I2C / UART.

Static checks on the firmware sources, as the rest of tests/ does: the
behaviour itself is pinned by the App Library's conformance tests, which run
the firmware's VmCore against the JavaScript VM.
"""

import re
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from _firmware import CORE_ROOT  # noqa: E402

REPO_ROOT = Path(__file__).resolve().parents[1]
FIRMWARE_ROOT = REPO_ROOT / "firmware" / "newhorizons_os"


def read(name: str) -> str:
    return (FIRMWARE_ROOT / name).read_text(encoding="utf-8")


def body(source: str, signature: str) -> str:
    start = source.index(signature)
    open_brace = source.index("{", start)
    depth = 0
    for i in range(open_brace, len(source)):
        if source[i] == "{":
            depth += 1
        elif source[i] == "}":
            depth -= 1
            if depth == 0:
                return source[open_brace : i + 1]
    raise AssertionError(f"unbalanced body for {signature}")


class GrantTests(unittest.TestCase):
    def test_dangerous_capabilities_need_the_operators_grant(self):
        contract = read("AppContract.h")
        dangerous = re.search(r"kAppDangerousCapabilities = ([^;]+);", contract).group(1)
        for cap in ("kAppCapStorage", "kAppCapDeviceConfig", "kAppCapDeviceStream",
                    "kAppCapIoI2c", "kAppCapIoUart"):
            self.assertIn(cap, dangerous)
        # Messages between apps are not dangerous: no consent asked.
        self.assertNotIn("kAppCapAppIpc", dangerous)

    def test_install_and_bind_both_check_the_grant(self):
        registry = read("AppRegistry.cpp")
        install = body(registry, "bool AppRegistry::install(")
        self.assertIn("AppPackage::checkGrant(manifest.capabilities, granted, error)", install)
        # Before the package is indexed or dry-run.
        self.assertLess(install.index("checkGrant"), install.index("AppVm::dryRun"))
        bind = body(registry, "bool AppRegistry::bind(")
        self.assertLess(bind.index("checkGrant"), bind.index("loadFromJson"))
        # The grant survives a reboot with the index.
        self.assertIn('"granted"', body(registry, "bool AppRegistry::loadIndex("))

    def test_app_install_parses_grant_by_name(self):
        handler = body(read("ControlCommandsApps.cpp"), "String ControlServer::cmdAppInstall(")
        self.assertIn('jsonExtractArray(request, "grant", grantArray)', handler)
        self.assertIn("AppPackage::capabilityMask(grantArray, granted, installError)", handler)

    def test_unsupported_io_is_refused_by_board(self):
        check = body(read("AppPackage.cpp"), "bool AppPackage::checkGrant(")
        self.assertIn("#if !NHOS_BOARD_HAS_APP_I2C", check)
        self.assertIn("#if !NHOS_BOARD_HAS_APP_UART", check)
        self.assertIn('"capability_unsupported:"', check)
        self.assertIn('"capability_not_granted:"', check)

    def test_only_the_f_boards_expose_app_io(self):
        config = (CORE_ROOT / "BoardConfig.h").read_text(encoding="utf-8")
        blocks = re.split(r"#(?:el)?if defined\((NHOS_BOARD_\w+)\)", config)
        exposed = {blocks[i] for i in range(1, len(blocks) - 1, 2)
                   if "#define NHOS_BOARD_HAS_APP_I2C  1" in blocks[i + 1]}
        self.assertEqual(exposed, {"NHOS_BOARD_V15F", "NHOS_BOARD_V20F"})
        self.assertIn("#define NHOS_BOARD_HAS_APP_I2C 0", config)
        self.assertIn("#define NHOS_BOARD_HAS_APP_UART 0", config)


class IoTests(unittest.TestCase):
    def test_the_boards_own_chips_are_denied(self):
        contract = read("AppContract.h")
        denied = re.search(r"kVmI2cDenied\[[^\]]*\] = \{([^}]*)\}", contract).group(1)
        addresses = {int(x, 16) for x in re.findall(r"0x[0-9A-Fa-f]+", denied)}
        # BMM350, MAX17048, OLED x2, PCA9536, MCP4725, BMI270, BQ25180.
        self.assertTrue({0x14, 0x36, 0x3C, 0x3D, 0x41, 0x60, 0x68, 0x6A} <= addresses)

    def test_i2c_transfers_are_held_to_a_short_timeout(self):
        io = read("AppIo.cpp")
        self.assertIn("Wire.setTimeOut(AppIo::kTimeoutMs)", io)
        self.assertIn("kTimeoutMs = 5", read("AppIo.h"))

    def test_the_uart_is_given_back_when_an_app_stops(self):
        vm = read("AppVm.cpp")
        release = body(vm, "void AppVm::releaseResources()")
        self.assertIn("AppIo::uartClose(this)", release)
        self.assertIn("AppDeviceControl::release(this)", release)
        self.assertIn("store_.flush()", release)
        self.assertIn("releaseResources();", body(vm, "void AppVm::stop()"))
        self.assertIn("releaseResources();", body(vm, "void AppVm::raiseFault("))
        self.assertIn("stop();", body(vm, "void AppVm::unload()"))
        close = body(read("AppIo.cpp"), "void AppIo::uartClose(")
        self.assertIn("Serial1.end()", close)
        self.assertIn("pinMode(NHOS_BOARD_APP_UART_TX, INPUT)", close)


class DeviceControlTests(unittest.TestCase):
    def test_only_whitelisted_settings_and_never_saved(self):
        control = read("AppDeviceControl.cpp")
        self.assertIn("kVmDeviceConfigKeys[i]", body(control, "int8_t AppDeviceControl::indexOf("))
        self.assertNotIn(".save(", control)
        self.assertNotIn("setScanTiming(", control)  # DeviceConfig keeps the operator's value
        self.assertIn("ConfigRegistry::find", body(control, "bool AppDeviceControl::set("))
        keys = re.search(r"kVmDeviceConfigKeys\[kVmDeviceConfigKeyCount\] = \{([^}]*)\}",
                         read("AppContract.h")).group(1)
        self.assertEqual(re.findall(r'"([^"]+)"', keys), [
            "scan.target_fps", "scan.send_every_n_frames", "filter.enabled",
            "filter.median", "filter.alpha", "imu.enabled"])

    def test_config_set_is_rate_limited(self):
        handler = body(read("AppVm.cpp"), "bool AppVm::vmConfigSet(")
        self.assertIn("kVmDeviceConfigIntervalMs", handler)

    def test_a_paused_stream_still_scans_and_runs_apps(self):
        sketch = read("newhorizons_os.ino")
        scan = sketch[sketch.index("void scanAndStreamIfDue()") :]
        self.assertLess(scan.index("lastFrameReady = true;"),
                        scan.index("AppDeviceControl::streamPaused()"))


class StoreTests(unittest.TestCase):
    def test_nvs_keys_fit(self):
        store = read("AppStore.cpp")
        self.assertIn('String key = "as_";', store)
        self.assertIn("substring(0, 12)", store)  # 3 + 12 = 15, NVS's limit

    def test_store_paths_fit_spiffs(self):
        storage = read("Storage.cpp")
        self.assertIn('root = "/a";', storage)
        # "/a/" + 15-char id + "/" + 8-char file + ".csv" = 31.
        self.assertEqual(len("/a/") + 15 + 1 + 8 + len(".csv"), 31)

    def test_uninstall_erases_the_store(self):
        uninstall = body(read("AppRegistry.cpp"), "bool AppRegistry::uninstall(")
        self.assertIn("AppStore::erase(*storage_, entry.manifest.id)", uninstall)

    def test_no_flash_write_in_the_timed_path_but_a_full_buffer(self):
        store = read("AppStore.cpp")
        append = body(store, "bool AppStore::append(")
        self.assertIn("kVmStoreQuotaBytes", append)
        self.assertNotIn("putBytes", append)
        self.assertIn("kVmStoreFlushMs", body(store, "void AppStore::service("))


class MessageTests(unittest.TestCase):
    def test_publish_reaches_other_running_apps_only(self):
        publish = body(read("AppManager.cpp"), "void AppManager::publish(")
        self.assertIn("AppState::Running", publish)
        self.assertIn("strcmp(slot.app->manifest().name, app) == 0", publish)
        self.assertIn("queueMessage(topic, value)", publish)

    def test_the_queue_is_bounded_and_counts_drops(self):
        queue = body(read("AppVm.cpp"), "void AppVm::queueMessage(")
        self.assertIn("kVmMessageQueue", queue)
        self.assertIn("++messagesDropped_", queue)


if __name__ == "__main__":
    unittest.main()
