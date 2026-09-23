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

    def test_budget_overruns_stop_the_app(self):
        impl = read("AppManager.cpp")
        header = read("AppManager.h")

        self.assertIn("kMaxConsecutiveOverruns = 5", header)
        # Killed when it blew an allocation nobody moved; Suspended when the
        # governor had just cut it. See HardwareRegressionTests for why that
        # distinction is not cosmetic.
        self.assertIn("slot.consecutiveOverruns >= kMaxConsecutiveOverruns", impl)
        self.assertIn("AppState::Killed", impl)

    def test_a_killed_app_is_not_silently_re_enabled(self):
        impl = read("AppManager.cpp")

        enable = impl[impl.index("bool AppManager::setEnabled") : impl.index("bool AppManager::revive")]
        self.assertIn("if (slot.state == AppState::Killed) {", enable)
        self.assertIn("return false;", enable)

    def test_capabilities_are_enforced_by_the_host_not_the_app(self):
        impl = read("AppManager.cpp")

        dispatch = impl[impl.index("void AppManager::dispatch") : impl.index("void AppManager::recordEvent")]
        # An app that never declared a capability is not handed the data, even
        # if some other bit subscribed it to the event.
        self.assertIn("kAppCapReadMatrix) == 0", dispatch)
        self.assertIn("delivered.frame = nullptr;", dispatch)
        self.assertIn("kAppCapReadImu) == 0", dispatch)
        self.assertIn("delivered.imuSample = nullptr;", dispatch)

    def test_subscription_and_permission_are_the_same_bit(self):
        impl = read("AppManager.cpp")

        # Two separate lists could disagree; one bit cannot. An app that may
        # not read the matrix is simply never woken for a frame.
        table = impl[impl.index("uint16_t subscriptionBit") : impl.index("}  // namespace")]
        self.assertIn("case AppEventKind::Frame: return kAppCapReadMatrix;", table)
        self.assertIn("case AppEventKind::Imu: return kAppCapReadImu;", table)

    def test_flow_graph_cost_is_checked_before_it_ever_runs(self):
        impl = read("FlowApp.cpp")

        parse = impl[impl.index("bool FlowApp::parse") : impl.index("bool FlowApp::loadFromFile")]
        self.assertIn("estimateUs(cellCount)", parse)
        self.assertIn("over_budget:", parse)
        # A rejected graph must not disturb the one already running.
        self.assertIn("nodeCount_ = previousCount;", parse)

    def test_flow_graphs_cannot_express_a_cycle(self):
        impl = read("FlowApp.cpp")

        parse = impl[impl.index("bool FlowApp::parse") : impl.index("bool FlowApp::loadFromFile")]
        self.assertIn("input_out_of_order", parse)
        # Every reference, including the multi-input forms, is checked against
        # the count of nodes parsed SO FAR.
        self.assertEqual(parse.count("value >= count") + parse.count("input >= count"), 2)

    def test_threshold_has_hysteresis_so_events_do_not_chatter(self):
        impl = read("FlowApp.cpp")

        self.assertIn("releaseAt", impl)
        self.assertIn("node.boolResult ? (input > releaseAt) : (input >= node.value)", impl)

    def test_gate_is_costed_at_its_worst_case(self):
        impl = read("FlowApp.cpp")

        estimate = impl[impl.index("uint32_t FlowApp::estimateUs") : impl.index("bool FlowApp::parse")]
        # Costing the average would understate the bound on exactly the frames
        # where the gate does not fire, so the estimator has no Gate case at
        # all -- every node is charged as if it runs.
        self.assertIn("never skips", estimate)
        self.assertNotIn("FlowOp::Gate", estimate)


class AppEventModelTests(unittest.TestCase):
    """Apps are their own scheduler entry, not passengers on the scan."""

    def test_apps_have_their_own_task(self):
        sketch = read("newhorizons_os.ino")
        self.assertIn('scheduler.registerTask("apps", &taskApps, false)', sketch)

    def test_the_apps_task_runs_after_the_scan_that_feeds_it(self):
        sketch = read("newhorizons_os.ino")
        # Registration order IS dispatch order, so the wrong order would hand
        # apps the PREVIOUS frame -- with no error, just data one frame stale.
        self.assertLess(sketch.index('registerTask("scan_stream"'),
                        sketch.index('registerTask("apps"'))

    def test_the_frame_is_handed_over_not_dispatched_inline(self):
        sketch = read("newhorizons_os.ino")
        scan = sketch[sketch.index("void scanAndStreamIfDue()") : sketch.index("void sendQueuedPacketIfAny()")]
        self.assertIn("lastFrameReady = true;", scan)
        self.assertNotIn("apps.dispatch(", scan)

    def test_dispatched_frames_carry_their_sequence(self):
        sketch = read("newhorizons_os.ino")
        task = sketch[sketch.index("void taskApps()") : sketch.index("void scanAndStreamIfDue()")]
        # frameSeq is what lets an app's output be lined up against recorded
        # samples afterwards.
        self.assertIn("event.frameSeq = lastFrame.seq;", task)

    def test_a_tick_only_app_does_not_depend_on_scanning(self):
        sketch = read("newhorizons_os.ino")
        task = sketch[sketch.index("void taskApps()") : sketch.index("void scanAndStreamIfDue()")]
        # The tick is emitted outside the lastFrameReady branch.
        self.assertGreater(task.index("AppEventKind::Tick"), task.index("lastFrameReady = false;"))

    def test_slot_identity_is_set_before_install(self):
        sketch = read("newhorizons_os.ino")
        # install() indexes by manifest().name and persists app_en_<name>, so a
        # slot renamed afterwards would be unreachable forever.
        self.assertLess(sketch.index("setIdentity(kSlotNames[i])"),
                        sketch.index("apps.install(slotPtrs[i])"))

    def test_the_flow_engine_has_per_instance_identity(self):
        header = read("FlowApp.h")
        impl = read("FlowApp.cpp")
        # A file-static manifest cannot name four coexisting instances, and
        # AppManager::install() rejects the second one by name.
        self.assertNotIn("const AppManifest kManifest", impl)
        self.assertIn("AppManifest manifest_;", header)
        self.assertIn("FlowApp(const FlowApp&) = delete;", header)

    def test_events_are_attributed_to_the_emitting_slot(self):
        impl = read("FlowApp.cpp")
        evaluate = impl[impl.index("void FlowApp::evaluate") : impl.index("void FlowApp::onEvent")]
        emit = evaluate[evaluate.index("case FlowOp::Emit:") : evaluate.index("case FlowOp::EmitValue:")]
        # Four slots share this class, so a file-static name would credit every
        # slot's events to the first one.
        self.assertIn("emitEvent(manifest_.name", emit)


class AppBudgetTests(unittest.TestCase):
    """Scanning wins. Apps are what gives way."""

    def test_the_governor_reads_scan_health_not_scheduler_load(self):
        impl = read("AppGovernor.cpp")
        header = read("AppGovernor.h")
        # A tick that performs a scan legitimately consumes most of a frame, so
        # busyPermille would fire constantly; overrunFrames is the authority on
        # missed sampling deadlines.
        self.assertIn("health.overrunFrames", impl)
        self.assertNotIn("busyPermille", impl)
        self.assertIn("NOT Scheduler::busyPermille()", header)

    def test_giving_ground_is_faster_than_taking_it_back(self):
        impl = read("AppGovernor.cpp")
        header = read("AppGovernor.h")
        self.assertIn("allowanceUs_ >> kDecreaseShift", impl)
        self.assertIn("ceilingUs_ / kIncreaseSteps", impl)
        # Multiplicative decrease, additive increase: the asymmetry is what
        # stops the loop oscillating.
        self.assertIn("kIncreaseSteps = 5", header)

    def test_the_floor_is_zero_apps_not_a_degraded_scan(self):
        impl = read("AppGovernor.cpp")
        self.assertIn("apply(reduced > 100 ? reduced : 0, nowMs)", impl)

    def test_suspension_is_not_a_kill(self):
        header = read("AppManager.h")
        impl = read("AppManager.cpp")
        # A suspension that needed an operator to clear it would mean one busy
        # moment takes an app away permanently.
        self.assertIn("Suspended = 4", header)
        budget = impl[impl.index("void AppManager::setTotalBudgetUs") : impl.index("void AppManager::reallocate")]
        self.assertIn("AppState::Suspended", budget)
        self.assertIn('recordEvent(slots_[i].app->manifest().name, "resumed"', budget)

    def test_a_resumption_does_not_flap(self):
        impl = read("AppGovernor.cpp")
        header = read("AppGovernor.h")
        self.assertIn("kMinSuspendMs", impl)
        self.assertIn("kMinSuspendMs = 5000", header)

    def test_allocation_is_divided_among_what_is_actually_running(self):
        impl = read("AppManager.cpp")
        realloc = impl[impl.index("void AppManager::reallocate") : impl.index("void AppManager::warnOverBudget")]
        self.assertIn("totalBudgetUs_ / running", realloc)

    def test_the_app_is_warned_before_it_is_stopped(self):
        impl = read("AppManager.cpp")
        warn = impl[impl.index("void AppManager::warnOverBudget") : impl.index("void AppManager::dispatch")]
        # Both audiences: the operator has levers the app does not, and the app
        # may shed work if it asked to be told.
        self.assertIn('recordEvent(manifest.name, "over_budget"', warn)
        self.assertIn("kAppCapBudget", warn)
        self.assertIn("AppEventKind::Budget", warn)

    def test_budget_events_are_not_charged_against_the_budget(self):
        impl = read("AppManager.cpp")
        dispatch = impl[impl.index("void AppManager::dispatch") : impl.index("void AppManager::recordEvent")]
        # Charging the runtime's own warning against the allocation it is
        # warning about would be circular.
        self.assertIn("if (event.kind == AppEventKind::Budget) {", dispatch)


class HardwareRegressionTests(unittest.TestCase):
    """Bugs that only showed up on a real device.

    Each of these passed every desk check and then failed on hardware, so each
    one gets an assertion rather than a memory.
    """

    def test_multi_input_parsing_skips_the_array_brackets(self):
        impl = read("FlowApp.cpp")
        # jsonExtractArray hands back the value WITH its brackets, so a parser
        # that stops at a non-digit stopped at '[' -- and every multi-input
        # operator (add, sub, mul, div, min, max, select, gate, emit_value)
        # silently had no inputs at all.
        parse = impl[impl.index("String inputArray;"):impl.index("float number = 0;")]
        self.assertIn("'['", parse)
        self.assertIn("']'", parse)

    def test_reindex_uses_paths_it_can_reopen(self):
        registry = read("AppRegistry.cpp")
        storage_h = read("Storage.h")
        # SPIFFS is flat, so the directory is part of the filename and
        # File::name() drops it: listFiles() reports "x.nha" for a file that
        # can only be opened as "apps/x.nha".
        self.assertIn("listFilePaths", registry)
        self.assertNotIn('listFiles("user")', registry)
        self.assertIn("std::vector<String> listFilePaths", storage_h)

    def test_listed_paths_are_relative_to_the_scope_root(self):
        impl = read("Storage.cpp")
        listing = impl[impl.index("std::vector<String> Storage::listFilePaths"):
                       impl.index("String Storage::listFiles")]
        # scopedPath(scope, "") has no trailing separator, so a strip of just
        # its length leaves a leading '/' that scopedPath cannot rebuild.
        self.assertIn('const String prefix = root + "/";', listing)

    def test_a_fruitless_rebuild_does_not_cement_an_empty_index(self):
        registry = read("AppRegistry.cpp")
        restore = registry[registry.index("void AppRegistry::restore"):
                           registry.index("String AppRegistry::statusJson")]
        # An empty index written by a failed rebuild loads cleanly forever,
        # so the device never tries to rebuild again.
        self.assertIn("persistWhenEmpty=*/false", restore)
        self.assertIn("if (recovered == 0 && !persistWhenEmpty)", registry)

    def test_a_cut_allocation_suspends_rather_than_kills(self):
        impl = read("AppManager.cpp")
        dispatch = impl[impl.index("void AppManager::dispatch"):
                        impl.index("void AppManager::recordEvent")]
        # Observed on hardware: the governor cut the allocation and two apps
        # were Killed for overrunning the new, smaller one. Killed needs an
        # operator, so a transient scan overload disabled them permanently.
        self.assertIn("throttled_ ? AppState::Suspended : AppState::Killed", dispatch)

    def test_moving_the_yardstick_clears_the_tally(self):
        impl = read("AppManager.cpp")
        setter = impl[impl.index("void AppManager::setTotalBudgetUs"):
                      impl.index("void AppManager::reallocate")]
        self.assertIn("slots_[i].consecutiveOverruns = 0;", setter)

    def test_an_unreachable_target_does_not_cost_the_apps_anything(self):
        impl = read("AppGovernor.cpp")
        update = impl[impl.index("void AppGovernor::update"):]
        # If one scan already takes longer than the whole frame period, no
        # amount of app-shedding can make the target reachable. Verified on
        # hardware: the scan rate was identical with apps running and with
        # every one of them suspended.
        self.assertIn("health.lastScanDurationUs >= periodUs", update)
        self.assertIn("kMinAppSharePermille", update)


class ReadoutPackageTests(unittest.TestCase):
    """A readout is stored by the device but never runs on it."""

    def test_the_kind_is_a_discriminator_not_an_assumption(self):
        impl = read("AppPackage.cpp")
        # An older firmware must refuse a kind it does not understand rather
        # than misread it as a flow graph.
        self.assertIn('kind != "flow" && !isReadout', impl)
        self.assertIn("unsupported_kind:", impl)

    def test_a_readout_is_granted_nothing(self):
        impl = read("AppPackage.cpp")
        # It runs nowhere on the device, so it needs no capability at all.
        self.assertIn("isReadout ? kAppCapNone :", impl)

    def test_a_readout_has_no_graph_to_dry_run(self):
        impl = read("AppRegistry.cpp")
        install = impl[impl.index("bool AppRegistry::install"):impl.index("bool AppRegistry::uninstall")]
        self.assertIn("manifest.kind != kAppPackageReadout", install)

    def test_a_readout_cannot_be_bound_to_a_slot(self):
        impl = read("AppRegistry.cpp")
        activate = impl[impl.index("bool AppRegistry::activate"):impl.index("bool AppRegistry::deactivate")]
        # Binding it would leave a slot that looks occupied but dispatches
        # nothing, which misleads anyone reading the roster.
        self.assertIn("not_activatable:readout", activate)

    def test_restore_never_binds_a_readout(self):
        impl = read("AppRegistry.cpp")
        restore = impl[impl.index("void AppRegistry::restore"):impl.index("String AppRegistry::statusJson")]
        self.assertIn("kAppPackageReadout", restore)

    def test_the_index_persists_the_kind(self):
        impl = read("AppRegistry.cpp")
        # Without this a readout comes back as a flow after a reboot, and the
        # device would then try to run it.
        save = impl[impl.index("bool AppRegistry::saveIndex"):impl.index("bool AppRegistry::loadIndex")]
        load = impl[impl.index("bool AppRegistry::loadIndex"):impl.index("void AppRegistry::restore")]
        self.assertIn('\\"kind\\":', save)
        self.assertIn('jsonExtractString(object, "kind", "flow")', load)


class IdleSlotTests(unittest.TestCase):
    """An enabled slot with no package is idle, not running.

    Seen after uninstalling every package: all four slots still said Running
    and each took a quarter of the budget, so one real app beside three empty
    slots would have been held to 25% of the ceiling.
    """

    def test_flow_slots_are_idle_without_a_graph(self):
        self.assertIn("virtual bool idle() const { return false; }", read("App.h"))
        self.assertIn("bool idle() const override { return !loaded(); }", read("FlowApp.h"))

    def test_idle_slots_take_no_share(self):
        impl = read("AppManager.cpp")
        count = impl[impl.index("uint8_t AppManager::runningCount"):impl.index("uint8_t AppManager::activeMask")]
        self.assertIn("!slots_[i].app->idle()", count)
        realloc = impl[impl.index("void AppManager::reallocate"):impl.index("void AppManager::warnOverBudget")]
        self.assertIn("!slots_[i].app->idle() ? share : 0", realloc)

    def test_idle_slots_are_not_dispatched(self):
        impl = read("AppManager.cpp")
        dispatch = impl[impl.index("void AppManager::dispatch"):]
        self.assertIn("slot.state != AppState::Running || slot.app->idle()", dispatch)

    def test_binding_or_removing_a_package_redivides_the_budget(self):
        impl = read("AppManager.cpp")
        dispatch = impl[impl.index("void AppManager::dispatch"):]
        head = dispatch[:dispatch.index("for (uint8_t i = 0; i < count_; ++i)")]
        self.assertIn("if (mask != activeMask_)", head)
        self.assertIn("reallocate();", head)

    def test_idle_is_reported(self):
        impl = read("AppManager.cpp")
        self.assertIn('json += "\\",\\"idle\\":";', impl)
        self.assertIn('"idle"', impl[impl.index('String out = "NAME'):])


class CompactStatusTests(unittest.TestCase):
    """A Direct-mode status has to fit one ESP-NOW reply (7680 B).

    The full status grew to ~13 KB with v1.1.0's app and scheduler sections,
    so over ESP-NOW every status came back response_too_large and the Desktop
    showed "unknown" for everything. Direct mode leaves out only sections no
    status consumer reads.
    """

    OMITTED = ["battery_gauge", "config", "ota_rollback", "faults", "scheduler",
               "airtime", "services", "power_governor", "apps", "magnetometer"]
    # Read from status by the Desktop frontend or backend; must never be dropped.
    KEPT = ["device_uid", "device_name", "protocol", "mode", "firmware_version",
            "hardware_model", "matrix_shape", "matrix_layout", "runtime", "wifi",
            "battery", "power", "logging", "ota", "update_state", "clock", "filter",
            "stream_raw_adc", "imu", "stream_buffer", "calibration", "indicators",
            "action_button", "scan_health", "findme"]

    def handler(self):
        impl = read("ControlServer.cpp")
        start = impl.index('if (cmd == "status" || cmd == "query")')
        return impl, impl[start:impl.index('return ok(cmd, "status", data);', start)]

    def test_compact_only_in_direct_mode(self):
        _, body = self.handler()
        self.assertIn("const bool compact = espNowOta_ != nullptr;", body)

    def test_omitted_sections_are_guarded_and_named(self):
        impl, body = self.handler()
        listing = impl[impl.index("kCompactStatusOmitted[] ="):]
        listing = listing[:listing.index(";")]
        for key in self.OMITTED:
            self.assertIn(f'if (!compact) jsonRawField(data, "{key}"', body)
            self.assertIn(f'\\"{key}\\"', listing)
        self.assertIn('jsonRawField(data, "omitted", kCompactStatusOmitted, first);', body)

    def test_fields_the_desktop_reads_are_always_sent(self):
        _, body = self.handler()
        for key in self.KEPT:
            line = next(l for l in body.splitlines() if f'(data, "{key}"' in l)
            self.assertNotIn("compact", line, key)


class ImuReadPathTests(unittest.TestCase):
    """One bus transaction per sample, with the Bosch compensation intact.

    Measured on v1.5.F in one session, 200 interleaved samples: the old
    per-sensor pair took 4568us, the single read 2272us, and the values agreed
    to a tenth of one gyro LSB -- including gyro X, which carries the chip's
    factory cross-axis correction.
    """

    def test_service_reads_once(self):
        impl = read("ImuManager.cpp")
        service = impl[impl.index("void ImuManager::service"):]
        # The library's per-sensor readers each fetch BOTH sensors and discard
        # half, so calling the pair read the device twice per sample.
        self.assertIn("imuDriver.readAccelGyro(acc, gyr)", service)
        self.assertNotIn("readGyroscope(", service)
        self.assertNotIn("readAcceleration(", service)

    def test_the_read_goes_through_the_bosch_api(self):
        driver = read("ImuDriver.cpp")
        # bmi2_get_sensor_data applies the factory cross-axis correction to
        # gyro X and the axis remap. A raw register read would silently drop
        # both; this asserts nobody "optimises" it into one.
        self.assertIn("bmi2_get_sensor_data(&data, dev_)", driver)
        self.assertNotIn("Wire.beginTransmission", driver)
        self.assertNotIn("requestFrom", driver)

    def test_the_scale_factors_match_the_library(self):
        driver = read("ImuDriver.cpp")
        self.assertIn("kInt16ToG = 8192.0f", driver)
        self.assertIn("kInt16ToDps = 16.384f", driver)

    def test_one_driver_instance_serves_both_managers(self):
        # On BMM150 boards the magnetometer is hosted by the same driver, so it
        # must be the instance that was begun.
        self.assertIn("imuDriver.readMagneticField(", read("MagnetometerManager.cpp"))
        self.assertIn("imuDriver.begin(", read("ImuManager.cpp"))
        for name in ("ImuManager.cpp", "MagnetometerManager.cpp"):
            with self.subTest(file=name):
                self.assertNotIn("IMU.", read(name))

    def test_the_unused_fifo_mode_is_gone(self):
        # It was enabled but never drained, so it bought nothing.
        self.assertNotIn("setContinuousMode()", read("ImuManager.cpp").replace(
            "No continuous (FIFO) mode", ""))


class CostModelTests(unittest.TestCase):
    def test_the_constants_are_the_measured_ones(self):
        header = read("FlowApp.h")
        # The originals were guesses and under-estimated the real cost by 1.4x
        # to 9x, which made the install-time estimate optimistic exactly where
        # an author relies on it.
        self.assertIn("kCellOpNsPerCell = 300", header)
        self.assertIn("kFeaturesNsPerCell = 500", header)
        self.assertIn("kScalarOpNs = 600", header)
        self.assertIn("MEASURED", header)


class AppRegistryTests(unittest.TestCase):
    def test_package_ids_fit_the_spiffs_path_cap(self):
        header = read("AppPackage.h")
        impl = read("AppPackage.cpp")
        # "/files/" + "apps/" + id + ".nha" must stay within 31 characters.
        self.assertIn("kIdLen = 16", header)
        self.assertIn("31-character path limit", impl)

    def test_a_package_is_validated_before_it_is_indexed(self):
        impl = read("AppRegistry.cpp")
        install = impl[impl.index("bool AppRegistry::install") : impl.index("bool AppRegistry::uninstall")]
        self.assertLess(install.index("parseManifest"), install.index("saveIndex()"))
        # Dry-run, so a broken package is refused at install rather than
        # discovered at activation.
        self.assertIn("graph_invalid:", install)
        self.assertLess(install.index("scratch.loadFromJson"), install.index("saveIndex()"))

    def test_the_manifest_id_must_match_the_filename(self):
        impl = read("AppRegistry.cpp")
        install = impl[impl.index("bool AppRegistry::install") : impl.index("bool AppRegistry::uninstall")]
        # Otherwise uninstall and reindex key on a name nothing can remove.
        self.assertIn("id_path_mismatch:", install)

    def test_index_writes_are_not_assumed_atomic(self):
        impl = read("AppRegistry.cpp")
        load = impl[impl.index("bool AppRegistry::loadIndex") : impl.index("void AppRegistry::restore")]
        # writeTextFileAtomic removes the target before renaming, so the tmp
        # file is the only evidence a torn write leaves behind.
        self.assertIn("kIndexTmpPath", load)

    def test_a_lost_index_is_rebuilt_from_the_packages(self):
        impl = read("AppRegistry.cpp")
        restore = impl[impl.index("void AppRegistry::restore") : impl.index("String AppRegistry::statusJson")]
        self.assertIn("reindex(", restore)

    def test_uninstall_writes_the_index_before_deleting_the_file(self):
        impl = read("AppRegistry.cpp")
        uninstall = impl[impl.index("bool AppRegistry::uninstall") : impl.index("bool AppRegistry::bind")]
        # An orphan file is recoverable; an index entry pointing at a deleted
        # file is not.
        self.assertLess(uninstall.index("saveIndex()"), uninstall.index("deleteFile"))

    def test_a_bad_graph_does_not_stop_the_device_booting(self):
        impl = read("AppRegistry.cpp")
        restore = impl[impl.index("void AppRegistry::restore") : impl.index("String AppRegistry::statusJson")]
        self.assertIn("entry.loadFailed = true;", restore)
        self.assertIn("app_restore_failed", restore)

    def test_a_package_cannot_widen_its_own_permissions(self):
        impl = read("FlowApp.cpp")
        apply = impl[impl.index("void FlowApp::applyPackageManifest") : impl.index("FlowOp FlowApp::opFromName")]
        self.assertIn("capabilities & allowed", apply)


if __name__ == "__main__":
    unittest.main()
