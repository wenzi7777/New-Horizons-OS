"""The command table (v1.15.0): ControlServer's 87-branch if-chain became
rows of {name, handler, flags, summary}. These pin what the table must keep."""
import re
import unittest

from _control_source import ARDUINO_ROOT, command_rows, command_names, control_source, flagged, handler_body

# Exactly the commands v1.14.0's if-chain answered, in its order.
V1_14_COMMANDS = [
    "status", "query", "sensor_sample", "memory_status", "task_list", "app_list", "app_view",
    "app_enable", "app_disable", "app_revive", "app_events", "app_load_flow", "app_unload_flow",
    "app_list_packages", "app_install", "app_uninstall", "app_activate", "app_deactivate",
    "app_verify", "app_reindex", "config_schema", "config_get", "config_set", "capabilities",
    "set_power_profile", "set_time", "clock_sync", "clock_status", "set_clock_config",
    "service_list", "service_restart", "scan_health", "storage_status", "enter_maintenance",
    "exit_maintenance", "set_scan_timing", "set_transport", "set_stream_buffer",
    "set_matrix_layout", "set_wifi", "findme_discover", "findme_switch_gateway",
    "set_charge_profile", "power_set_state", "set_action_button", "set_filter", "set_raw_adc",
    "set_battery_profile", "battery_status", "resync_battery_gauge", "set_imu", "set_log",
    "set_indicators", "calibration_status", "calibration_enable", "calibration_disable",
    "calibration_session_begin", "calibration_session_abort", "calibration_session_commit",
    "calibration_clear_profile", "calibration_dump_tare", "calibration_dump_level",
    "calibration_dump_fit", "calibration_set_fit", "calibration_set_reference",
    "calibration_delete_level", "calibration_capture_tare", "calibration_tare_capture",
    "calibration_tare_clear", "calibration_capture_cell", "calibration_capture_all", "file_list",
    "file_read_begin", "file_read_chunk", "file_write_begin", "file_write_chunk",
    "file_write_finish", "file_delete", "log_tail", "dmesg", "log_clear", "crash_log", "health",
    "crash_clear", "set_ota_config", "check_update", "apply_update", "reboot", "reboot_wifi_setup",
]

# v1.14.0 gated these inside their bodies (two idioms); exit_maintenance joins
# them on purpose -- it was a silent no-op in normal mode.
MAINTENANCE = {
    "app_install", "app_uninstall", "app_activate", "app_deactivate", "app_reindex",
    "calibration_session_begin", "calibration_session_abort", "calibration_session_commit",
    "calibration_clear_profile", "calibration_set_fit", "calibration_set_reference",
    "calibration_delete_level", "calibration_capture_tare", "calibration_capture_cell",
    "calibration_capture_all", "file_write_begin", "file_write_chunk", "file_write_finish",
    "file_delete", "exit_maintenance",
}


class CommandTableTests(unittest.TestCase):
    def test_same_commands_as_before_plus_commands(self):
        names = command_names()
        self.assertEqual(len(names), len(set(names)), "duplicate rows")
        self.assertEqual([n for n in names if n != "commands"], V1_14_COMMANDS)
        self.assertIn("commands", names)

    def test_flags(self):
        self.assertEqual(flagged("kCmdNeedsMaintenance"), MAINTENANCE)
        self.assertEqual(flagged("kCmdQuietLed"), {"app_view", "app_events"})
        self.assertEqual(flagged("kCmdHeavy"), {"status", "query", "storage_status", "commands"})
        self.assertEqual(flagged("kCmdInternal"), {"set_transport", "set_wifi"})
        self.assertEqual(flagged("kCmdStatusSnapshot"),
                         {"status", "query", "memory_status", "scan_health", "storage_status"})

    def test_every_row_has_a_summary_and_a_defined_handler(self):
        source = control_source()
        for row in command_rows():
            self.assertTrue(row["summary"], row["name"])
            self.assertEqual(source.count(f"String ControlServer::{row['handler']}(const CommandContext& ctx) {{"), 1,
                             row["name"])

    def test_aliases_share_a_handler(self):
        handlers = {row["name"]: row["handler"] for row in command_rows()}
        self.assertEqual(handlers["status"], handlers["query"])
        self.assertEqual(handlers["app_enable"], handlers["app_disable"])

    def test_maintenance_is_enforced_once_by_the_dispatcher(self):
        source = control_source()
        dispatch = source[source.index("String ControlServer::processCommand(const String& request) {"):]
        dispatch = dispatch[:dispatch.index("\n}\n")]
        self.assertIn("(spec->flags & kCmdNeedsMaintenance) != 0 && !maintenanceMode()", dispatch)
        self.assertIn('return error(cmd, "unknown_command");', dispatch)
        self.assertNotIn("requireMaintenance", source)
        for name in MAINTENANCE:
            self.assertNotIn('"maintenance_required"', handler_body(name), name)

    def test_commands_and_capabilities_describe_the_table(self):
        self.assertIn('return ok(ctx.cmd, "commands", commandsJson());', handler_body("commands"))
        capabilities = handler_body("capabilities")
        self.assertIn("command_schema_hash", capabilities)
        self.assertIn("command_count", capabilities)
        table = (ARDUINO_ROOT / "ControlCommandTable.cpp").read_text(encoding="utf-8")
        self.assertIn("uint32_t hash = 2166136261u;", table)  # FNV-1a, like ConfigRegistry

    def test_the_dispatch_if_chain_is_gone(self):
        self.assertIsNone(re.search(r'^\s*if \(cmd == "', control_source(), re.M))


if __name__ == "__main__":
    unittest.main()
