import unittest
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[1]
FIRMWARE_ROOT = REPO_ROOT / "firmware" / "newhorizons_os"


def read(name: str) -> str:
    return (FIRMWARE_ROOT / name).read_text(encoding="utf-8")


class AirtimeArbiterTests(unittest.TestCase):
    def test_arbiter_is_the_only_caller_of_esp_now_send(self):
        offenders = []
        for path in FIRMWARE_ROOT.glob("*.cpp"):
            if path.name == "AirtimeArbiter.cpp":
                continue
            for number, line in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
                stripped = line.strip()
                if stripped.startswith("//"):
                    continue
                if "esp_now_send(" in stripped:
                    offenders.append("{}:{}".format(path.name, number))
        self.assertEqual(offenders, [], "esp_now_send() must go through AirtimeArbiter::send()")

    def test_there_is_no_api_to_send_a_burst_unpaced(self):
        header = read("AirtimeArbiter.h")

        # The whole point: a fragment array can only be handed over to be
        # paced, never blasted out immediately.
        self.assertIn("beginPacedBurst", header)
        self.assertNotIn("sendBurstNow", header)
        self.assertNotIn("sendAll", header)

    def test_ota_hub_request_no_longer_loops_over_send(self):
        impl = read("EspNowOtaReceiver.cpp")

        request = impl[impl.index("void EspNowOtaReceiver::sendHubRequest") : impl.index("void EspNowOtaReceiver::sendManifestRequest")]
        self.assertIn("beginPacedBurstFromBytes", request)
        self.assertNotIn("for (uint8_t i = 0; i < count; ++i)", request)

    def test_streaming_gate_uses_the_priority_table(self):
        sketch = read("newhorizons_os.ino")

        gate = sketch[sketch.index("bool streamingGateOk()") : sketch.index("void scanAndStreamIfDue()")]
        self.assertIn("airtime.canClaim(nhos::AirtimeClass::SensorStream)", gate)
        # The two hand-written exclusions it replaced must be gone.
        self.assertNotIn("espNowOtaReceiver.isRelaying()", gate)
        self.assertNotIn("espNowPairing.hasPendingCommandWork()", gate)

    def test_claimants_are_polled_so_a_claim_cannot_leak(self):
        impl = read("AirtimeArbiter.cpp")

        self.assertIn("claimant.fn(claimant.ctx)", impl)
        self.assertNotIn("tryClaim", impl)
        self.assertNotIn("release(", impl)


class ServiceManagerTests(unittest.TestCase):
    def test_backoff_is_bounded_and_gives_up(self):
        impl = read("ServiceManager.cpp")
        header = read("ServiceManager.h")

        self.assertIn("kMaxBackoffMs = 300000", header)
        self.assertIn("kMaxAttempts = 5", header)
        self.assertIn("service.state = ServiceState::Failed", impl)

    def test_non_restartable_services_are_never_retried(self):
        impl = read("ServiceManager.cpp")

        retry = impl[impl.index("void ServiceManager::scheduleRetry") : impl.index("void ServiceManager::attemptRestart")]
        self.assertIn("if (service.start == nullptr)", retry)

    def test_operator_restart_clears_the_failed_verdict(self):
        impl = read("ServiceManager.cpp")

        restart = impl[impl.index("bool ServiceManager::restart") : impl.index("const char* ServiceManager::stateName")]
        self.assertIn("service.attempts = 0", restart)

    def test_disabled_imu_is_healthy_not_broken(self):
        sketch = read("newhorizons_os.ino")

        probe = sketch[sketch.index("bool imuHealthy(void*)") : sketch.index("bool imuStart(void*)")]
        self.assertIn("!deviceConfig.data().imuEnabled || imu.initialized()", probe)


class LoggingTests(unittest.TestCase):
    def test_ring_is_filled_even_when_flash_logging_is_off(self):
        impl = read("Storage.cpp")

        tagged = impl[impl.index("void Storage::logTagged") : impl.index("bool Storage::tagAllows")]
        self.assertLess(tagged.index("pushRing("), tagged.index("if (!logEnabled_)"))

    def test_untagged_logging_still_works(self):
        impl = read("Storage.cpp")

        self.assertIn('logTagged("sys", line, level)', impl)


class ClockTests(unittest.TestCase):
    def test_a_garbage_push_cannot_destroy_a_good_clock(self):
        impl = read("TimeSync.cpp")

        setter = impl[impl.index("bool TimeSync::setEpochMs") : impl.index("String TimeSync::statusJson")]
        self.assertIn("kTimeSyncValidEpochS", setter)
        self.assertIn("return false", setter)

    def test_drift_is_measured_before_the_clock_is_stepped(self):
        impl = read("TimeSync.cpp")

        setter = impl[impl.index("bool TimeSync::setEpochMs") : impl.index("String TimeSync::statusJson")]
        self.assertLess(setter.index("lastAdjustMs_ ="), setter.index("settimeofday"))

    def test_set_time_needs_no_new_espnow_message(self):
        control = read("ControlServer.cpp")

        # It is an ordinary control command, so it rides whichever transport
        # already reaches the device.
        self.assertIn('if (cmd == "set_time")', control)
        self.assertNotIn("kEspNowFragTypeTime", read("EspNowFrame.h"))


class PowerGovernorTests(unittest.TestCase):
    def test_clock_is_never_lowered_while_scanning(self):
        impl = read("PowerGovernor.cpp")

        target = impl[impl.index("uint32_t PowerGovernor::targetMhz") : impl.index("void PowerGovernor::applyMhz")]
        self.assertIn("if (scannerActive) {", target)
        self.assertIn("return kFullMhz;", target)

    def test_default_profile_preserves_existing_behaviour(self):
        header = read("PowerGovernor.h")
        self.assertIn("PowerProfile profile_ = PowerProfile::Performance;", header)

    def test_governor_owns_every_frequency_change(self):
        sketch = read("newhorizons_os.ino")
        self.assertNotIn("setCpuFrequencyMhz(", sketch)


class ConfigRegistryTests(unittest.TestCase):
    def test_writes_reuse_the_existing_validated_handlers(self):
        control = read("ControlServer.cpp")

        setter = control[control.index('if (cmd == "config_set")') : control.index('if (cmd == "capabilities")')]
        self.assertIn("entry->command", setter)
        self.assertIn("return processCommand(synthesized);", setter)
        # Recursion must be provably depth-1.
        self.assertIn('strncmp(entry->command, "config_", 7)', setter)

    def test_every_entry_names_a_real_command(self):
        registry = read("ConfigRegistry.cpp")
        control = read("ControlServer.cpp")

        import re

        commands = set(re.findall(r'ConfigType::\w+, "(\w+)"', registry))
        self.assertGreater(len(commands), 5)
        for command in commands:
            self.assertIn('cmd == "{}"'.format(command), control, command)

    def test_schema_hash_tracks_shape_not_values(self):
        registry = read("ConfigRegistry.cpp")

        hasher = registry[registry.index("uint32_t ConfigRegistry::schemaHash") :]
        self.assertIn("kEntries[i].path", hasher)
        self.assertIn("kEntries[i].argument", hasher)


class SchedulerMetricTests(unittest.TestCase):
    def test_no_second_deadline_alarm_competing_with_scan_health(self):
        """Regression: found on hardware.

        The tick budget was the scan interval, so a tick that performed a scan
        legitimately consumed most of it -- 4083 of 4085 ticks were counted as
        "overruns" while ScanHealth::overrunFrames, the real deadline
        authority, read 0. Load is now reported as busy_permille and
        attributed per task; the alarm is gone.
        """
        header = read("Scheduler.h")
        impl = read("Scheduler.cpp")
        sketch = read("newhorizons_os.ino")

        for banned in ("tickOverruns", "tickBudgetUs", "setTickBudgetUs"):
            self.assertNotIn(banned, header, banned)
            self.assertNotIn(banned, impl, banned)
        self.assertNotIn("setTickBudgetUs", sketch)
        self.assertIn("busyPermille", header)
        self.assertIn("busy_permille", impl)


class ConfigSetHardwareRegressionTests(unittest.TestCase):
    def test_numbers_and_bools_are_not_sent_quoted(self):
        """Regression: found on hardware.

        Every value used to be emitted as a JSON string. The handlers'
        extractors take the raw text up to the next delimiter and strtol() it,
        so a quoted "30" failed to parse, silently fell back to the argument's
        compile-time default, and config_set still reported success.
        """
        registry = read("ConfigRegistry.cpp")

        encode = registry[registry.index("bool ConfigRegistry::encodeValue") : registry.index("uint32_t ConfigRegistry::schemaHash")]
        self.assertIn('out = "true"', encode)
        self.assertIn('out = "false"', encode)
        self.assertIn("out = String(parsed);", encode)
        # Only text and enum get quoted.
        self.assertIn("case ConfigType::Enum:", encode)
        self.assertIn("expected_int", encode)
        self.assertIn("out_of_range:", encode)

    def test_a_single_field_write_preserves_its_siblings(self):
        """Regression: found on hardware.

        set_* handlers default an argument they did not receive to its
        compile-time default, not to its current value. Writing
        scan.target_fps alone would therefore also snap settle_us and
        send_every_n_frames back to defaults.
        """
        control = read("ControlServer.cpp")

        setter = control[control.index('if (cmd == "config_set")') : control.index('if (cmd == "capabilities")')]
        self.assertIn("ConfigRegistry::entries()", setter)
        self.assertIn("strcmp(sibling.command, entry->command)", setter)
        self.assertIn("ConfigRegistry::readValue(String(sibling.path)", setter)

    def test_an_unparseable_value_is_rejected_not_defaulted(self):
        control = read("ControlServer.cpp")

        setter = control[control.index('if (cmd == "config_set")') : control.index('if (cmd == "capabilities")')]
        self.assertIn("ConfigRegistry::encodeValue(*entry, value, encoded, encodeError)", setter)
        self.assertIn("value_rejected:", setter)
        self.assertLess(setter.index("encodeValue"), setter.index("processCommand(synthesized)"))


class AppFrameworkTests(unittest.TestCase):
    def test_apps_are_opt_in(self):
        impl = read("AppManager.cpp")

        install = impl[impl.index("bool AppManager::install") : impl.index("String AppManager::enabledKey")]
        self.assertIn("slot.state = AppState::Installed;", install)
        self.assertIn("enabledKey", install)

    def test_budget_overruns_kill_the_app(self):
        impl = read("AppManager.cpp")
        header = read("AppManager.h")

        self.assertIn("kMaxConsecutiveOverruns = 5", header)
        self.assertIn("slot.state = AppState::Killed;", impl)

    def test_a_killed_app_is_not_silently_re_enabled(self):
        impl = read("AppManager.cpp")

        enable = impl[impl.index("bool AppManager::setEnabled") : impl.index("bool AppManager::revive")]
        self.assertIn("if (slot.state == AppState::Killed) {", enable)
        self.assertIn("return false;", enable)

    def test_capabilities_are_enforced_by_the_host_not_the_app(self):
        impl = read("AppManager.cpp")

        dispatch = impl[impl.index("void AppManager::onFrame") : impl.index("void AppManager::emitEvent")]
        self.assertIn("kAppCapReadMatrix) != 0 ? &frame : nullptr", dispatch)
        self.assertIn("kAppCapReadImu) != 0 ? imuSample : nullptr", dispatch)

    def test_rule_graph_cost_is_checked_before_it_ever_runs(self):
        impl = read("RuleEngineApp.cpp")

        parse = impl[impl.index("bool RuleEngineApp::parse") : impl.index("bool RuleEngineApp::loadFromFile")]
        self.assertIn("estimateUs(cellCount)", parse)
        self.assertIn("over_budget:", parse)
        # A rejected graph must not disturb the one already running.
        self.assertIn("nodeCount_ = previousCount;", parse)

    def test_rule_graphs_cannot_express_a_cycle(self):
        impl = read("RuleEngineApp.cpp")

        parse = impl[impl.index("bool RuleEngineApp::parse") : impl.index("bool RuleEngineApp::loadFromFile")]
        self.assertIn("input >= count", parse)
        self.assertIn("input_out_of_order", parse)

    def test_threshold_has_hysteresis_so_events_do_not_chatter(self):
        impl = read("RuleEngineApp.cpp")

        self.assertIn("releaseAt", impl)
        self.assertIn("node.boolResult ? (input > releaseAt) : (input >= node.value)", impl)

    def test_apps_run_where_the_frame_exists(self):
        sketch = read("newhorizons_os.ino")

        scan = sketch[sketch.index("void scanAndStreamIfDue()") : sketch.index("void sendQueuedPacketIfAny()")]
        self.assertIn("apps.onFrame(frame,", scan)
        self.assertLess(scan.index("apps.onFrame("), scan.index("buildMatrixPacketHeader"))


if __name__ == "__main__":
    unittest.main()
