import importlib.util
import struct
import sys
import types
import unittest
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[1]


def load_packet_module():
    fake_config = types.SimpleNamespace(
        USE_HMAC=False,
        HMAC_LEN=0,
        DEVICE_NAME="New Horizons OS",
        ACTIVE_ROWS=[1],
        ACTIVE_COLS=[13, 14],
        ACTIVE_SENSOR_COUNT=None,
        PACKET_FORMAT="BINARY",
        MAGIC=0xA55A,
        PACKET_VERSION=2,
        BMI270_ACC_DECIMALS=3,
        BMI270_GYRO_DECIMALS=3,
        BMI270_TEMP_DECIMALS=2,
        TEXT_MATRIX_WITH_LABEL=True,
        TEXT_LINE_HMAC=False,
        TEXT_END_MARKER="END",
    )
    fake_identity = types.SimpleNamespace(
        get_device_id=lambda: "3CDC7545CCD0",
        get_packet_device_uid_bytes=lambda: bytes.fromhex("3CDC7545CCD0"),
        get_device_name=lambda default: default,
    )
    injected = {
        "config": fake_config,
        "crypto_hmac": types.SimpleNamespace(hmac_sha256=lambda key, body: b""),
        "device_identity": fake_identity,
    }
    old_modules = {}
    for name, module in injected.items():
        old_modules[name] = sys.modules.get(name)
        sys.modules[name] = module
    try:
        spec = importlib.util.spec_from_file_location("packet_builder_test_module", REPO_ROOT / "device" / "os" / "packet.py")
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
        return module, old_modules
    except Exception:
        restore_modules(old_modules)
        raise


def restore_modules(old_modules):
    for name, old in old_modules.items():
        if old is None:
            sys.modules.pop(name, None)
        else:
            sys.modules[name] = old


class PacketBuilderTests(unittest.TestCase):
    def test_binary_packet_header_uses_full_mac_uid(self):
        module, old_modules = load_packet_module()
        self.addCleanup(restore_modules, old_modules)

        packet = module.PacketBuilder().build(7, 1234, [1.0, 2.0])

        self.assertEqual(struct.unpack_from("<HBB", packet, 0), (0xA55A, 2, 0))
        self.assertEqual(packet[4:10], bytes.fromhex("3CDC7545CCD0"))
        self.assertEqual(struct.unpack_from("<IIH", packet, 10), (7, 1234, 8))
        self.assertEqual(struct.unpack_from("<2f", packet, 20), (1.0, 2.0))


if __name__ == "__main__":
    unittest.main()
