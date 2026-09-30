import re
import unittest
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[1]
FIRMWARE_ROOT = REPO_ROOT / "firmware" / "newhorizons_os"


def read(name: str) -> str:
    return (FIRMWARE_ROOT / name).read_text(encoding="utf-8")


class LifetimeStatsTests(unittest.TestCase):
    """SMART-style lifetime counters: the `health` command and /proc/health."""

    def test_health_command_is_served_and_advertised(self):
        control = read("ControlServer.cpp")

        self.assertIn('if (cmd == "health")', control)
        self.assertIn('return ok(cmd, "health", lifetime_->healthJson());', control)
        self.assertIn('data += ",\\"health\\":";', control)

    def test_health_stays_out_of_status(self):
        # The compact (ESP-NOW) status has ~1.9 KB of headroom left; lifetime
        # counters are fetched on their own instead.
        control = read("ControlServer.cpp")
        start = control.index('if (cmd == "status" || cmd == "query")')
        body = control[start:control.index('return ok(cmd, "status", data);', start)]
        self.assertNotIn("lifetime_", body)
        self.assertNotIn('"health"', body)

    def test_proc_health_is_listed(self):
        proc = read("ProcFs.cpp")

        entries = proc[proc.index("kEntries[] = {"):]
        entries = entries[:entries.index("};")]
        self.assertIn('"health"', entries)
        self.assertIn('if (path == "health")', proc)
        self.assertIn("lifetime_->procText()", proc)

    def test_every_restart_path_flushes_through_the_shutdown_handler(self):
        impl = read("LifetimeStats.cpp")

        # One hook instead of a flush() before each of the ESP.restart() call
        # sites (reboot, OTA apply, pairing fallback, WiFi portal, ...).
        self.assertIn("esp_register_shutdown_handler(&LifetimeStats::onShutdown);", impl)
        on_shutdown = impl[impl.index("void LifetimeStats::onShutdown()"):]
        on_shutdown = on_shutdown[:on_shutdown.index("\n}\n")]
        self.assertIn("->flush();", on_shutdown)

    def test_soft_off_entry_and_interval_flush(self):
        impl = read("LifetimeStats.cpp")
        service = impl[impl.index("void LifetimeStats::service("):]
        service = service[:service.index("\n}\n")]

        self.assertIn("wasAwake_ && !awake", service)
        self.assertIn("lifetimeFlushDue(nowMs, lastFlushMs_)", service)
        policy = read("LifetimePolicy.h")
        self.assertIn("kLifetimeFlushIntervalMs = 10UL * 60UL * 1000UL;", policy)

    def test_nvs_names_fit_the_15_char_limit(self):
        impl = read("LifetimeStats.cpp")
        for name in ("kNamespace", "kStatsKey"):
            match = re.search(r'constexpr char %s\[\] = "([^"]+)";' % name, impl)
            self.assertIsNotNone(match, name)
            self.assertLessEqual(len(match.group(1)), 15, name)
        # Its own namespace, like FaultRecorder's nhos_fault.
        self.assertIn('constexpr char kNamespace[] = "nhos_life";', impl)

    def test_counters_have_no_reset_command(self):
        control = read("ControlServer.cpp")
        self.assertNotIn('cmd == "health_clear"', control)
        self.assertNotIn('cmd == "health_reset"', control)

    def test_task_is_always_run_at_1hz_before_soft_off(self):
        sketch = read("newhorizons_os.ino")
        block = sketch[sketch.index("void registerRuntimeTasks()"):sketch.index("}  // namespace")]

        self.assertIn('scheduler.registerTask("lifetime", &taskLifetime, true, 1000000);', block)
        self.assertLess(block.index('"lifetime"'), block.index('"soft_off"'))

    def test_begin_follows_fault_recorder_and_boot_mode(self):
        sketch = read("newhorizons_os.ino")
        setup = sketch[sketch.index("void setup() {"):]

        begin_at = setup.index("lifetime.begin(faults, bootMode);")
        self.assertGreater(begin_at, setup.index("faults.begin();"))
        self.assertGreater(begin_at, setup.index("bootMode.begin();"))

    def test_ota_confirm_and_rollback_are_counted(self):
        sketch = read("newhorizons_os.ino")
        confirm = sketch[sketch.index("if (bootMode.confirmFirmwareValid()) {"):]
        self.assertLess(confirm.index("lifetime.noteOtaConfirmed();"), confirm.index("}"))

        boot = read("BootModeManager.cpp")
        revert = boot[boot.index('prefs_.putString("rb_from", pending);'):]
        self.assertLess(revert.index("rolledBackThisBoot_ = true;"), revert.index("}"))
        self.assertIn("boot.rolledBackThisBoot()", read("LifetimeStats.cpp"))

    def test_button_count_is_board_gated(self):
        impl = read("LifetimeStats.cpp")
        for chunk in re.findall(r"#if NHOS_BOARD_HAS_BUTTON(.*?)#endif", impl, re.S):
            self.assertIn("button_presses", chunk)
        self.assertEqual(impl.count("button_presses"), 2)

    def test_crash_total_reuses_fault_recorder_classification(self):
        impl = read("LifetimeStats.cpp")
        self.assertIn("FaultRecorder::isCrashReason(i)", impl)
        self.assertIn("FaultRecorder::resetReasonName(i)", impl)

    def test_every_reset_reason_has_its_own_name(self):
        # reset_counts is keyed by name, so two reasons sharing "unknown"
        # would emit a duplicate JSON key.
        impl = read("FaultRecorder.cpp")
        names = impl[impl.index("const char* FaultRecorder::resetReasonName"):]
        names = names[:names.index("\n}\n")]
        for reason in ("USB", "JTAG", "EFUSE", "PWR_GLITCH", "CPU_LOCKUP"):
            self.assertIn("case ESP_RST_%s:" % reason, names)


if __name__ == "__main__":
    unittest.main()
