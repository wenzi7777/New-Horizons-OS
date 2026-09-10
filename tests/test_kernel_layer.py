import unittest
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[1]
FIRMWARE_ROOT = REPO_ROOT / "firmware" / "newhorizons_os"


def read(name: str) -> str:
    return (FIRMWARE_ROOT / name).read_text(encoding="utf-8")


class OtaRollbackTests(unittest.TestCase):
    def test_sketch_defers_rollback_decision_away_from_arduino_core(self):
        sketch = read("newhorizons_os.ino")

        # Without this the core confirms a fresh image inside initArduino(),
        # before a single line of NHOS has run.
        self.assertIn('extern "C" bool verifyRollbackLater()', sketch)
        self.assertIn("return true;", sketch)

    def test_image_is_confirmed_only_after_a_boot_that_reached_runtime(self):
        sketch = read("newhorizons_os.ino")

        confirm_at = sketch.index("bootMode.confirmFirmwareValid()")
        runtime_ready_at = sketch.index('runtime_ready protocol=')
        self.assertGreater(confirm_at, runtime_ready_at)
        self.assertIn("bootMode.mode() != nhos::RunMode::SafeMaintenance && !criticalError", sketch)

    def test_auto_ota_never_chains_off_an_unconfirmed_image(self):
        sketch = read("newhorizons_os.ino")

        auto_ota = sketch[sketch.index("void serviceAutoOta(") : sketch.index("String chargeStateName()")]
        self.assertIn("bootMode.otaPendingVerify()", auto_ota)
        self.assertIn("auto_ota_deferred", auto_ota)

    def test_boot_mode_manager_reconstructs_rollback_state(self):
        impl = read("BootModeManager.cpp")

        self.assertIn("esp_ota_mark_app_valid_cancel_rollback()", impl)
        self.assertIn("esp_ota_get_state_partition", impl)
        self.assertIn("ESP_OTA_IMG_PENDING_VERIFY", impl)
        self.assertIn('prefs_.putString("pend_ver"', impl)
        self.assertIn('prefs_.putString("rb_from"', impl)


class WatchdogTests(unittest.TestCase):
    def test_loop_watchdog_is_armed_after_the_blocking_part_of_boot(self):
        sketch = read("newhorizons_os.ino")

        arm_at = sketch.index("nhos::watchdogArm();\n  logBoot")
        auto_ota_at = sketch.index("serviceAutoOta(wifiConnected);")
        self.assertGreater(arm_at, auto_ota_at)

    def test_soft_off_disarms_rather_than_racing_the_five_second_sleep(self):
        sketch = read("newhorizons_os.ino")
        power_state = read("PowerStateManager.h")

        # The race this guards against only exists because the two are equal.
        self.assertIn("kSoftOffBatterySleepUs = 5000000ULL", power_state)
        suspend = sketch[
            sketch.index("void suspendRuntimeServicesForSoftOff()") : sketch.index("void applySoftOffIndicators()")
        ]
        self.assertIn("nhos::watchdogDisarm();", suspend)

    def test_unbounded_loops_feed_the_watchdog(self):
        scanner = read("MatrixScanner.cpp")
        ota = read("OtaManager.cpp")
        wifi = read("WifiManager.cpp")

        # durationMs is operator-supplied and never clamped.
        self.assertEqual(scanner.count("watchdogFeed()"), 2)
        self.assertIn("watchdogFeed();", ota)
        self.assertIn("watchdogFeed();", wifi)

    def test_feeding_before_the_watchdog_is_armed_is_a_no_op(self):
        """Regression: found on hardware.

        WifiManager's 8s association loop and OtaManager's download loop both
        also run inside setup(), before enableLoopWDT() has subscribed the
        loop task. A naked feedLoopWDT() there makes esp_task_wdt_reset()
        return ESP_ERR_NOT_FOUND and the IDF log an error at 20Hz, which threw
        away every boot line after magnetometer_ready.
        """
        impl = read("Watchdog.cpp")

        feed = impl[impl.index("void watchdogFeed()") :]
        self.assertIn("if (!g_armed) {", feed)
        self.assertIn("return;", feed)

        # No module may call the unguarded Arduino helpers directly.
        for name in ("WifiManager.cpp", "OtaManager.cpp", "MatrixScanner.cpp",
                     "newhorizons_os.ino"):
            source = read(name)
            self.assertNotIn("feedLoopWDT(", source, name)
            self.assertNotIn("enableLoopWDT(", source, name)
            self.assertNotIn("disableLoopWDT(", source, name)


    def test_unfeedable_blocking_calls_pause_the_watchdog(self):
        """Regression: found by audit after the hardware session.

        HTTPClient::GET() and Update::end() are single blocking calls with no
        loop to feed from, and they allow 6-24s of their own. With the
        watchdog armed they reboot the device mid-check_update / mid-OTA --
        a regression the watchdog itself introduced, since before it these
        were merely slow.
        """
        ota = read("OtaManager.cpp")
        header = read("Watchdog.h")

        self.assertIn("class WatchdogPause", header)
        # RAII, because these functions are full of early returns.
        self.assertIn("~WatchdogPause()", header)

        manifest = ota[ota.index("bool OtaManager::fetchManifest") : ota.index("bool OtaManager::downloadAndApply")]
        self.assertIn("WatchdogPause", manifest)

        download = ota[ota.index("bool OtaManager::downloadAndApply") :]
        self.assertIn("WatchdogPause watchdogPause;\n    code = http.GET();", download)
        self.assertIn("WatchdogPause finalizePause;", download)

    def test_no_blocking_http_call_is_left_unguarded(self):
        ota = read("OtaManager.cpp")

        # Every http.GET() must sit inside a pause scope.
        self.assertEqual(ota.count("http.GET()"), 2)
        for index in [i for i in range(len(ota)) if ota.startswith("http.GET()", i)]:
            window = ota[max(0, index - 400):index]
            self.assertIn("WatchdogPause", window)


class FaultRecorderTests(unittest.TestCase):
    def test_core_dump_is_read_not_reconfigured(self):
        impl = read("FaultRecorder.cpp")

        # default_8MB.csv already carries a coredump partition and the
        # prebuilt libs already write to it; nothing needs enabling.
        self.assertIn("esp_core_dump_image_check()", impl)
        self.assertIn("esp_core_dump_get_summary(&summary)", impl)
        self.assertIn("summary.exc_task", impl)
        self.assertIn("summary.exc_pc", impl)

    def test_intentional_resets_are_not_recorded_as_crashes(self):
        impl = read("FaultRecorder.cpp")

        reasons = impl[impl.index("bool FaultRecorder::isCrashReason") : impl.index("const char* FaultRecorder::resetReasonName")]
        for crash in ("ESP_RST_PANIC", "ESP_RST_INT_WDT", "ESP_RST_TASK_WDT", "ESP_RST_BROWNOUT"):
            self.assertIn(crash, reasons)
        # ESP.restart() must not look like a crash.
        self.assertNotIn("ESP_RST_SW:", reasons)

    def test_control_server_exposes_crash_commands(self):
        control = read("ControlServer.cpp")

        self.assertIn('if (cmd == "crash_log")', control)
        self.assertIn('if (cmd == "crash_clear")', control)
        self.assertIn('jsonRawField(data, "faults"', control)


class SchedulerTests(unittest.TestCase):
    def test_registration_order_matches_the_previous_loop_order(self):
        sketch = read("newhorizons_os.ino")

        block = sketch[sketch.index("void registerRuntimeTasks()") : sketch.index("}  // namespace")]
        order = [
            "power", "battery_gauge", "power_state", "imu", "magnetometer",
            "scan_stream", "stream_queue", "control", "led",
            "power_transition", "display", "soft_off",
        ]
        positions = [block.index('"%s"' % name) for name in order]
        self.assertEqual(positions, sorted(positions))

    def test_soft_off_runs_last_and_always(self):
        sketch = read("newhorizons_os.ino")

        block = sketch[sketch.index("void registerRuntimeTasks()") : sketch.index("}  // namespace")]
        self.assertIn('scheduler.registerTask("soft_off", &taskSoftOff, true);', block)
        self.assertGreater(block.index('"soft_off"'), block.index('"display"'))

    def test_loop_is_just_the_tick_plus_the_unchanged_idle_heuristic(self):
        sketch = read("newhorizons_os.ino")

        loop = sketch[sketch.index("void loop() {") :]
        self.assertIn("scheduler.tick();", loop)
        self.assertIn("scanner.nextScanDueUs() - micros()) > 2000", loop)
        # Everything else must now live in the table.
        self.assertNotIn("control.service();", loop)
        self.assertNotIn("updateLedState();", loop)

    def test_gate_is_evaluated_per_task_not_once_per_tick(self):
        impl = read("Scheduler.cpp")

        tick = impl[impl.index("void Scheduler::tick()") : impl.index("void Scheduler::closeCpuWindowIfDue")]
        self.assertIn("!task.alwaysRun && gate_ != nullptr && !gate_()", tick)

    def test_scheduler_uses_no_freertos_primitives(self):
        impl = read("Scheduler.cpp") + read("Scheduler.h")

        for banned in ("xTaskCreate", "vTaskDelay", "SemaphoreHandle", "QueueHandle"):
            self.assertNotIn(banned, impl)


class ProcFsTests(unittest.TestCase):
    def test_proc_adds_no_new_command_surface(self):
        control = read("ControlServer.cpp")

        # It rides file_list / file_read_begin / file_read_chunk.
        self.assertIn("ProcFs::isProcScope(scope)", control)
        self.assertIn("proc_->list()", control)
        self.assertIn("proc_->read(path, offset, length, bytes)", control)

    def test_proc_scope_rejects_writes_and_deletes(self):
        control = read("ControlServer.cpp")

        self.assertEqual(control.count('return error(cmd, "read_only_scope");'), 2)

    def test_core_dump_is_reachable_as_a_file(self):
        proc = read("ProcFs.cpp")

        self.assertIn('"crash.elf"', proc)
        self.assertIn("faults_->readCoreDump(offset, out.data(), length, readBytes)", proc)


if __name__ == "__main__":
    unittest.main()
