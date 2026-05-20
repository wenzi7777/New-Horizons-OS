import unittest
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[1]
NATIVE_SCAN_PATH = REPO_ROOT / "firmware" / "native" / "vdboard" / "scan.c"


class NativeStreamMemoryTests(unittest.TestCase):
    def test_calibration_table_is_allocated_compactly(self):
        source = NATIVE_SCAN_PATH.read_text()

        self.assertIn("calibration_offsets", source)
        self.assertIn("calibration_point_capacity", source)
        self.assertNotIn("point_count * VDBOARD_STREAM_MAX_CAL_POINTS", source)
        self.assertIn("calibration_point_capacity * sizeof(vdboard_stream_cal_point_t)", source)

    def test_native_stream_can_pop_packet_into_preallocated_buffer(self):
        source = NATIVE_SCAN_PATH.read_text()

        self.assertIn("vdboard_stream_pop_packet_into", source)
        self.assertIn("mp_get_buffer_raise", source)
        self.assertIn("MP_QSTR_pop_packet_into", source)
        self.assertNotIn("return mp_obj_new_bytes(packet, total_len);", source)

    def test_scan_task_uses_heap_payload_buffer_not_large_stack_array(self):
        source = NATIVE_SCAN_PATH.read_text()

        self.assertIn("payload_mv", source)
        self.assertIn("scan_payload_bytes", source)
        self.assertNotIn("uint16_t payload_mv[VDBOARD_SCAN_MAX_ROWS * VDBOARD_SCAN_MAX_COLS];", source)

    def test_stream_alloc_failure_releases_scan_storage(self):
        source = NATIVE_SCAN_PATH.read_text()

        self.assertIn("static void vdboard_scan_release_storage(void);", source)
        self.assertIn("vdboard_scan_release_storage();\n        mp_raise_msg(&mp_type_MemoryError, MP_ERROR_TEXT(\"stream buffer alloc failed\"));", source)


if __name__ == "__main__":
    unittest.main()
