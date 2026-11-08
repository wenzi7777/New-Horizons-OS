"""System monitor (v1.19.0): sysmon snapshot, FreeRTOS threads, and operator
stop/start/period for scheduler tasks, services and apps.

Two promises are pinned here. Protection lives in the firmware, not the UI.
Nothing an operator stops survives a reboot.
"""
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from _control_source import command_names, control_source, handler_body  # noqa: E402
from _firmware import read_firmware as read  # noqa: E402


def between(text: str, start: str, end: str) -> str:
    return text[text.index(start) : text.index(end, text.index(start))]


class TaskControlTests(unittest.TestCase):
    def test_a_stopped_task_is_not_dispatched(self):
        impl = read("Scheduler.cpp")
        dispatch = between(impl, "bool Scheduler::dispatch(", "void Scheduler::closeCpuWindowIfDue")
        self.assertIn("if (!task.enabled || task.operatorStopped)", dispatch)

    def test_always_run_and_protected_tasks_refuse_stop_and_period(self):
        impl = read("Scheduler.cpp")
        for fn, end in (
            ("Scheduler::OpResult Scheduler::operatorStop(", "Scheduler::OpResult Scheduler::operatorStart("),
            ("Scheduler::OpResult Scheduler::operatorSetPeriod(", "Scheduler::OpResult Scheduler::operatorResetPeriod("),
        ):
            body = between(impl, fn, end)
            self.assertIn("if (task.isProtected || task.alwaysRun)", body)
            self.assertIn("return OpResult::Protected;", body)

    def test_the_lifelines_are_protected(self):
        sketch = read("newhorizons_os.ino")
        block = between(sketch, "void registerRuntimeTasks()", "}  // namespace")
        protected = between(block, "kProtectedTasks[] = {", "};")
        for name in ("control", "control_udp", "wifi", "espnow", "heartbeat", "stream_queue",
                     "power_transition"):
            self.assertIn('"%s"' % name, protected)
        # Set after the tasks exist: setProtected looks the name up.
        self.assertGreater(block.index("scheduler.setProtected(name)"), block.rindex("registerTask("))
        self.assertIn('scheduler.setFixedPeriod("scan_stream");', block)

    def test_period_is_bounded_and_resettable(self):
        header = read("Scheduler.h")
        self.assertIn("kMaxOperatorPeriodUs = 10000000", header)
        impl = read("Scheduler.cpp")
        self.assertIn("task.defaultPeriodUs = periodUs;", impl)
        self.assertIn("task.periodUs = task.defaultPeriodUs;", impl)
        body = handler_body("task_set_period")
        self.assertIn('== "default"', body)
        # A reset is not a back door around protection.
        self.assertIn("isProtected(name.c_str())", body)

    def test_status_reports_state_and_protection(self):
        impl = read("Scheduler.cpp")
        for field in ('\\"state\\"', '\\"protected\\"', '\\"default_period_us\\"', '\\"fixed_period\\"'):
            self.assertIn(field, impl)


class ServiceControlTests(unittest.TestCase):
    def test_stopping_is_opt_in_per_service(self):
        header = read("ServiceManager.h")
        self.assertIn("StopFn stop = nullptr", header)
        impl = read("ServiceManager.cpp")
        stop = between(impl, "ServiceManager::OpResult ServiceManager::stop(", "ServiceManager::OpResult ServiceManager::start(")
        self.assertIn("if (service.stop == nullptr)", stop)
        self.assertIn("return OpResult::NotStoppable;", stop)
        self.assertIn("service.state = ServiceState::Stopped;", stop)

    def test_only_scanner_and_imu_are_stoppable(self):
        sketch = read("newhorizons_os.ino")
        block = between(sketch, "void registerServices()", "void registerRuntimeTasks()")
        self.assertIn("&scannerStop", block)
        self.assertIn("&imuStop", block)
        self.assertEqual(block.count("Stop);"), 2)

    def test_supervision_leaves_a_stopped_service_alone(self):
        impl = read("ServiceManager.cpp")
        poll = between(impl, "void ServiceManager::service(", "bool ServiceManager::restart(")
        # Health and retry only act on Running and Degraded.
        self.assertIn("case ServiceState::Stopped:", poll)
        restart = between(impl, "bool ServiceManager::restart(", "ServiceManager::OpResult ServiceManager::stop(")
        self.assertIn("service.operatorStopped = false;", restart)

    def test_nothing_brings_a_stopped_module_back_behind_the_operator(self):
        sketch = read("newhorizons_os.ino")
        resume = between(sketch, "void applyNormalRuntimeState()", "void servicePowerTransition()")
        self.assertIn('!services.isOperatorStopped("imu")', resume)
        self.assertIn('!services.isOperatorStopped("scanner")', resume)
        self.assertIn('isOperatorStopped("scanner")', handler_body("exit_maintenance"))
        self.assertIn('isOperatorStopped("scanner")', handler_body("set_matrix_layout"))
        # set_imu enabled=true is an explicit start, not a silent override.
        self.assertIn('services_->start("imu")', handler_body("set_imu"))

    def test_imu_stop_does_not_touch_the_persisted_switch(self):
        sketch = read("newhorizons_os.ino")
        stop = between(sketch, "void imuStop(void*)", "\n}")
        self.assertNotIn("deviceConfig", stop)


class AppControlTests(unittest.TestCase):
    def test_app_stop_is_runtime_only(self):
        impl = read("AppManager.cpp")
        stop = between(impl, "AppManager::OpResult AppManager::stop(", "AppManager::OpResult AppManager::start(")
        start = between(impl, "AppManager::OpResult AppManager::start(", "const char* AppManager::opResultCode(")
        for body in (stop, start):
            self.assertNotIn("putUInt", body)
            self.assertNotIn("storage_", body)
        self.assertIn("slot.state = AppState::Stopped;", stop)
        self.assertIn("if (slot.state != AppState::Stopped)", start)
        # The governor resumes Suspended only; a stopped app stays stopped.
        budget = between(impl, "void AppManager::setTotalBudgetUs(", "void AppManager::reallocate()")
        self.assertNotIn("AppState::Stopped", budget)

    def test_app_list_finally_reports_the_governor(self):
        self.assertIn('"governor\\":"', control_source().replace("\\\\", "\\"))


class NothingPersistedTests(unittest.TestCase):
    def test_operator_controls_never_write_storage(self):
        for name in ("Scheduler.cpp", "ServiceManager.cpp"):
            impl = read(name)
            self.assertNotIn("Storage", impl)
            self.assertNotIn("putUInt", impl)
        for command in ("task_stop", "task_set_period", "service_stop", "app_stop"):
            body = handler_body(command)
            self.assertNotIn("save(", body)
            self.assertNotIn("storage_", body)


class SnapshotTests(unittest.TestCase):
    def test_commands_exist(self):
        names = command_names()
        for name in ("sysmon", "thread_list", "task_stop", "task_start", "task_set_period",
                     "service_stop", "service_start", "app_stop", "app_start"):
            self.assertIn(name, names)

    def test_sysmon_carries_everything_in_one_reply(self):
        body = handler_body("sysmon")
        for key in ('"scheduler"', '"services"', '"apps"', '"memory"', '"scan"', '"threads"', '"airtime"'):
            self.assertIn(key, body)

    def test_threads_are_read_only_and_kept_out_of_the_scheduler(self):
        monitor = read("ThreadMonitor.cpp") + read("ThreadMonitor.h")
        self.assertIn("uxTaskGetSystemState", monitor)
        for banned in ("vTaskSuspend", "vTaskDelete", "vTaskPrioritySet"):
            self.assertNotIn(banned, monitor)
        # CPU is a delta; the first sample reports null instead of inventing one.
        self.assertIn('strcpy(cpu, "null")', monitor)
        self.assertIn("runTime - previous_[p].runTime", monitor)


class PanelPackageTests(unittest.TestCase):
    def test_a_panel_is_stored_like_a_readout(self):
        header = read("AppPackage.h")
        self.assertIn("kAppPackagePanel = 2", header)
        self.assertIn("kind == kAppPackageReadout || kind == kAppPackagePanel", header)
        impl = read("AppPackage.cpp")
        self.assertIn('kind == "readout" || kind == "panel"', impl)


if __name__ == "__main__":
    unittest.main()
