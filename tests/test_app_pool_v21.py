"""v2.1.0: no app slots. How many apps run is whatever the pool and the time
budget admit; apps are named by package id and kept in an operator's order.

Static checks on the firmware sources, as the rest of tests/ does. The pool,
load and lending arithmetic is pinned by tests_native/test_app_resources.cpp.
"""

import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from _control_source import flagged, handler_body  # noqa: E402

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
                return source[start : i + 1]
    raise AssertionError(f"unbalanced: {signature}")


class NoSlotsTests(unittest.TestCase):
    def test_no_slot_limit_anywhere(self):
        for name in ("AppRegistry.h", "AppRegistry.cpp", "AppManager.h", "AppManager.cpp",
                     "newhorizons_os.ino", "ControlCommandsApps.cpp"):
            source = read(name)
            for gone in ("kMaxSlots", "kMaxApps", "slotOwner_", "freeSlot(", "no_free_slot",
                         "slot_occupied", "slotName("):
                self.assertNotIn(gone, source, f"{gone} in {name}")

    def test_apps_are_created_in_the_pool(self):
        sketch = read("newhorizons_os.ino")
        self.assertNotIn("nhos::AppVm flowApps", sketch)
        self.assertNotIn("apps/flow.json", sketch)
        registry = read("AppRegistry.cpp")
        bind = body(registry, "bool AppRegistry::bind(")
        self.assertIn("AppVm::create(*storage_)", bind)
        unbind = body(registry, "void AppRegistry::unbind(")
        # Out of the run list before it is freed.
        self.assertLess(unbind.index("apps_->remove(entry.app)"), unbind.index("AppVm::destroy(entry.app)"))

    def test_a_failed_bind_leaves_nothing_in_the_pool(self):
        bind = body(read("AppRegistry.cpp"), "bool AppRegistry::bind(")
        # Every refusal after create() gives the app back.
        self.assertEqual(bind.count("AppVm::destroy(app);"), 2)
        self.assertIn('error = "app_pool_exhausted";', bind)

    def test_the_store_is_taken_only_with_the_capability(self):
        vm = read("AppVm.cpp")
        apply = body(vm, "bool AppVm::applyPackageManifest(")
        self.assertIn("(manifest_.capabilities & kAppCapStorage) != 0", apply)
        self.assertIn("pool().allocateFixed(storeBytes())", apply)
        self.assertIn("releaseStore();", body(vm, "void AppVm::unload()"))
        self.assertIn("AppStore* store_ = nullptr;", read("AppVm.h"))
        memory = body(vm, "uint32_t AppVm::memoryFor(")
        self.assertIn("kAppCapStorage", memory)


class AdmissionTests(unittest.TestCase):
    def test_time_is_admitted_against_the_governor_ceiling(self):
        bind = body(read("AppRegistry.cpp"), "bool AppRegistry::bind(")
        self.assertIn("governor_->ceilingUs()", bind)
        self.assertIn("totalEstimatedUs() + entry.estimatedUs > ceiling", bind)
        self.assertIn('"app_budget_exhausted:"', bind)
        # An app with loops has no static cost and is not refused for time.
        self.assertIn("entry.estimatedUs != 0", bind)

    def test_activate_appends_to_the_order(self):
        activate = body(read("AppRegistry.cpp"), "bool AppRegistry::activate(")
        self.assertIn("bind(entry, nextOrder(), json, error)", activate)
        self.assertIn("unbind(entry);", activate)  # index write failed


class OrderTests(unittest.TestCase):
    def test_reorder_needs_no_maintenance(self):
        self.assertNotIn("app_reorder", flagged("kCmdNeedsMaintenance"))
        self.assertIn("app_activate", flagged("kCmdNeedsMaintenance"))
        handler = handler_body("app_reorder")
        self.assertIn('extractInt(request, "to", 0)', handler)

    def test_reorder_renumbers_and_resorts(self):
        reorder = body(read("AppRegistry.cpp"), "bool AppRegistry::reorder(")
        self.assertIn("entry.app->run.order = i;", reorder)
        self.assertIn("apps_->sortByOrder();", reorder)
        self.assertIn("saveIndex()", reorder)

    def test_the_run_list_stays_in_order(self):
        manager = read("AppManager.cpp")
        add = body(manager, "bool AppManager::add(")
        self.assertIn("(*link)->run.order <= order", add)
        display = body(manager, "bool AppManager::displayLine(")
        self.assertIn("for (const App* app = head_;", display)


class IndexMigrationTests(unittest.TestCase):
    def test_an_old_slot_reads_as_active_in_slot_order(self):
        load = body(read("AppRegistry.cpp"), "bool AppRegistry::loadIndex(")
        self.assertIn('jsonExtractBool(object, "active", flag)', load)
        old = load[load.index('jsonExtractInt(object, "slot", number)'):]
        self.assertIn("entry.active = true;", old)
        self.assertIn("entry.order = static_cast<uint16_t>(number);", old)

    def test_the_new_index_is_written(self):
        save = body(read("AppRegistry.cpp"), "bool AppRegistry::saveIndex(")
        for field in ('\\"active\\":', '\\"order\\":', '\\"memory_bytes\\":', '\\"burst\\":'):
            self.assertIn(field, save)
        self.assertNotIn('\\"slot\\":', save)

    def test_restore_goes_in_order_and_renumbers(self):
        restore = body(read("AppRegistry.cpp"), "void AppRegistry::restore(")
        self.assertIn("entries_[i].order < entries_[pick].order", restore)
        self.assertIn("bind(entry, nextOrder, json, error)", restore)

    def test_enable_keys_are_gone(self):
        self.assertNotIn("app_en_", read("AppManager.cpp"))


class ReportingTests(unittest.TestCase):
    def test_app_list_reports_resources(self):
        manager = body(read("AppManager.cpp"), "String AppManager::statusJson(")
        for field in ('\\"memory_bytes\\":', '\\"burst\\":', '\\"order\\":'):
            self.assertIn(field, manager)
        load = body(read("AppManager.cpp"), "void appendLoad(")
        for field in ('\\"load\\":', '\\"level\\":', '\\"cpu10\\":', '\\"borrowed_us\\":', '\\"lent_us\\":'):
            self.assertIn(field, load)
        monitor = read("ControlCommandsMonitor.cpp")
        # (Sliced, not brace-matched: the body has a "}" in a string.)
        listing = monitor[monitor.index("String ControlServer::appListJson("):monitor.index("String ControlServer::cmdSysmon(")]
        self.assertIn('\\"pool\\":{\\"bytes\\":', listing)
        registry = body(read("AppRegistry.cpp"), "String AppRegistry::statusJson(")
        for field in ('\\"pool_bytes\\":', '\\"pool_free\\":', '\\"budget_us\\":', '\\"memory_bytes\\":'):
            self.assertIn(field, registry)

    def test_proc_apps_has_load_columns(self):
        text = body(read("AppManager.cpp"), "String AppManager::appsText(")
        self.assertIn("CPU%  LOAD", text)


class LendingTests(unittest.TestCase):
    def test_overruns_are_judged_against_the_guaranteed_share(self):
        dispatch = body(read("AppManager.cpp"), "void AppManager::dispatch(")
        self.assertIn("run.baseUs > slice ? run.baseUs : slice", dispatch)
        self.assertIn("run.load.charge(elapsedUs, slice);", dispatch)

    def test_only_burst_apps_borrow(self):
        settle = body(read("AppManager.cpp"), "void AppManager::settle(")
        self.assertIn("app->manifest().burst && hungry", settle)
        self.assertIn("run.allocatedUs = base - run.lentUs + run.borrowedUs;", settle)

    def test_a_budget_change_drops_every_loan(self):
        realloc = body(read("AppManager.cpp"), "void AppManager::reallocate(")
        self.assertIn("app->run.borrowedUs = 0;", realloc)
        self.assertIn("app->run.lentUs = 0;", realloc)
        self.assertIn("reallocate();", body(read("AppManager.cpp"), "void AppManager::setTotalBudgetUs("))

    def test_burst_is_parsed_from_the_package(self):
        self.assertIn('jsonExtractBool(manifest, "burst", burst)', read("AppPackage.cpp"))


class StopTests(unittest.TestCase):
    def test_stopping_drops_a_suspended_run(self):
        # Hardware 2026-11-15: app_stop mid-loop, then app_start, resumed the
        # old run and was faulted starved at once.
        stop = body(read("AppVm.cpp"), "void AppVm::stop(")
        self.assertLess(stop.index("machine_.abandon();"), stop.index("machine_.hasEntry(VmEvent::Stop)"))
        self.assertIn("suspendedSinceMs_ = 0;", stop)
        self.assertIn("void abandon()", read("VmCore.h"))


if __name__ == "__main__":
    unittest.main()
