import struct


PROTOCOL = "NHCP/1"
TLV_MAGIC = b"NHTLV1\x00"
MAGIC = b"NH"
VERSION = 1
FLAG_ACK_REQUIRED = 0x01
FLAG_ERROR = 0x02
HEADER = "<2sBBBHHI6sH"
HEADER_SIZE = struct.calcsize(HEADER)

TYPE_IDS = {
    "ack": 1,
    "hello": 2,
    "status": 3,
    "command": 4,
    "result": 5,
    "update_progress": 6,
    "findme_discover": 7,
    "findme_offer": 8,
}
TYPE_NAMES = {value: key for key, value in TYPE_IDS.items()}

V_NULL = 0
V_FALSE = 1
V_TRUE = 2
V_INT = 3
V_STR = 4
V_BYTES = 5
V_LIST = 6
V_DICT = 7
V_FLOAT = 8
V_COMMAND = 9

FIELD_IDS = {
    "command": 1,
    "request_id": 2,
    "status": 3,
    "message": 4,
    "error": 5,
    "reboot_required": 6,
    "applied": 7,
    "device_uid": 8,
    "mode": 9,
    "runtime": 10,
    "scan_health": 11,
    "resource_guard": 12,
    "memory": 13,
    "update_state": 14,
    "progress": 15,
    "target_mode": 16,
    "expires_at_ms": 17,
    "device_name": 18,
    "runtime_version": 19,
    "recovery_version": 20,
    "os_version": 21,
    "wifi_rssi": 22,
    "preferred_gateway_id": 23,
    "claim_id": 24,
    "gateway_id": 25,
    "gateway_name": 26,
    "udp_port": 28,
    "priority": 29,
    "accept": 30,
    "upstream_status": 31,
    "ttl_ms": 32,
    "server_time": 33,
    "reason": 34,
    "cooldown_ms": 35,
}
FIELD_NAMES = {value: key for key, value in FIELD_IDS.items()}

COMMAND_IDS = {
    "status": 1,
    "query": 2,
    "memory_status": 3,
    "scan_health": 4,
    "set_matrix_layout": 5,
    "set_scan_timing": 6,
    "set_filter": 7,
    "set_indicators": 8,
    "findme_discover": 9,
    "reboot": 10,
    "reboot_to_recovery": 11,
    "reboot_to_os": 12,
    "check_os_release": 13,
    "write_os": 14,
    "check_recovery_release": 15,
    "write_recovery": 16,
    "release_recovery_resources": 17,
    "enter_maintenance": 18,
    "exit_maintenance": 19,
}
COMMAND_NAMES = {value: key for key, value in COMMAND_IDS.items()}


class NHCPError(Exception):
    pass


def encode_frame(msg_type, device_uid=b"", payload=None, seq=0, ack=0, flags=0, request_id=None):
    payload = {} if payload is None else payload
    body = encode_value(payload)
    if len(body) > 65535:
        raise NHCPError("payload_too_large")
    request_key = request_hash(request_id if request_id is not None else _request_id(payload))
    header = struct.pack(
        HEADER,
        MAGIC,
        VERSION,
        int(TYPE_IDS.get(msg_type, 0)),
        int(flags or 0) & 0xff,
        int(seq or 0) & 0xffff,
        int(ack or 0) & 0xffff,
        int(request_key or 0) & 0xffffffff,
        uid_bytes(device_uid),
        len(body),
    )
    packet = header + body
    return packet + struct.pack("<H", crc16(packet))


def encode_tlv(value):
    return TLV_MAGIC + encode_value(value)


def decode_tlv(data):
    if isinstance(data, str):
        data = data.encode("utf-8")
    if not isinstance(data, (bytes, bytearray)):
        raise NHCPError("invalid_tlv_payload")
    data = bytes(data)
    if not data.startswith(TLV_MAGIC):
        raise NHCPError("unsupported_tlv")
    value, offset = decode_value(data, len(TLV_MAGIC))
    if offset != len(data):
        raise NHCPError("trailing_tlv")
    return value


def decode_frame(packet):
    if len(packet) < HEADER_SIZE + 2:
        raise NHCPError("packet_too_short")
    expected = struct.unpack_from("<H", packet, len(packet) - 2)[0]
    data = packet[:-2]
    if crc16(data) != expected:
        raise NHCPError("crc_mismatch")
    magic, version, type_id, flags, seq, ack, request_key, device_uid, body_len = struct.unpack_from(HEADER, data, 0)
    if magic != MAGIC or int(version) != VERSION:
        raise NHCPError("unsupported_protocol")
    if len(data) != HEADER_SIZE + int(body_len):
        raise NHCPError("invalid_payload_length")
    value, offset = decode_value(data, HEADER_SIZE)
    if offset != len(data):
        raise NHCPError("trailing_payload")
    return {
        "protocol": PROTOCOL,
        "type": TYPE_NAMES.get(int(type_id), "unknown"),
        "flags": int(flags),
        "seq": int(seq),
        "ack": int(ack),
        "request_key": int(request_key),
        "device_uid": hex_uid(device_uid),
        "payload": value,
    }


def is_frame(packet):
    return bool(packet and len(packet) >= 2 and packet[:2] == MAGIC)


def encode_value(value):
    if value is None:
        return bytes([V_NULL])
    if value is False:
        return bytes([V_FALSE])
    if value is True:
        return bytes([V_TRUE])
    if isinstance(value, int):
        return bytes([V_INT]) + _pack_i64(int(value))
    if isinstance(value, float):
        return bytes([V_FLOAT]) + struct.pack("<f", float(value))
    if isinstance(value, (bytes, bytearray)):
        raw = bytes(value)
        return bytes([V_BYTES]) + struct.pack("<I", len(raw)) + raw
    if isinstance(value, (list, tuple)):
        parts = [bytes([V_LIST]), struct.pack("<I", len(value))]
        for item in value:
            parts.append(encode_value(item))
        return b"".join(parts)
    if isinstance(value, dict):
        items = list(value.items())
        parts = [bytes([V_DICT]), struct.pack("<I", len(items))]
        for key, item in items:
            key_text = str(key)
            field_id = int(FIELD_IDS.get(key_text, 0) or 0)
            parts.append(bytes([field_id & 0xff]))
            if not field_id:
                key_bytes = key_text.encode("utf-8")
                if len(key_bytes) > 255:
                    raise NHCPError("key_too_large")
                parts.append(bytes([len(key_bytes)]))
                parts.append(key_bytes)
            if key_text == "command" and isinstance(item, str):
                parts.append(encode_command(item))
            else:
                parts.append(encode_value(item))
        return b"".join(parts)
    raw = str(value).encode("utf-8")
    return bytes([V_STR]) + struct.pack("<I", len(raw)) + raw


def decode_value(data, offset=0):
    if offset >= len(data):
        raise NHCPError("missing_value")
    value_type = data[offset]
    offset += 1
    if value_type == V_NULL:
        return None, offset
    if value_type == V_FALSE:
        return False, offset
    if value_type == V_TRUE:
        return True, offset
    if value_type == V_INT:
        _require(data, offset, 8)
        return _unpack_i64(data, offset), offset + 8
    if value_type == V_FLOAT:
        _require(data, offset, 4)
        return float(struct.unpack_from("<f", data, offset)[0]), offset + 4
    if value_type in (V_STR, V_BYTES):
        _require(data, offset, 4)
        length = int(struct.unpack_from("<I", data, offset)[0])
        offset += 4
        _require(data, offset, length)
        raw = data[offset:offset + length]
        offset += length
        if value_type == V_BYTES:
            return raw, offset
        return raw.decode("utf-8"), offset
    if value_type == V_LIST:
        _require(data, offset, 4)
        count = int(struct.unpack_from("<I", data, offset)[0])
        offset += 4
        result = []
        for _idx in range(count):
            item, offset = decode_value(data, offset)
            result.append(item)
        return result, offset
    if value_type == V_DICT:
        _require(data, offset, 4)
        count = int(struct.unpack_from("<I", data, offset)[0])
        offset += 4
        result = {}
        for _idx in range(count):
            _require(data, offset, 1)
            field_id = int(data[offset])
            offset += 1
            if field_id:
                key = FIELD_NAMES.get(field_id, "field_{}".format(field_id))
            else:
                _require(data, offset, 1)
                key_len = int(data[offset])
                offset += 1
                _require(data, offset, key_len)
                key = data[offset:offset + key_len].decode("utf-8")
                offset += key_len
            item, offset = decode_value(data, offset)
            result[key] = item
        return result, offset
    if value_type == V_COMMAND:
        _require(data, offset, 1)
        command_id = int(data[offset])
        return COMMAND_NAMES.get(command_id, "command_{}".format(command_id)), offset + 1
    raise NHCPError("unknown_value_type")


def encode_command(command):
    command_id = int(COMMAND_IDS.get(str(command or ""), 0) or 0)
    if command_id:
        return bytes([V_COMMAND, command_id & 0xff])
    return encode_value(command)


def request_hash(value):
    raw = str(value or "").encode("utf-8")
    if not raw:
        return 0
    result = 2166136261
    for byte in raw:
        result ^= int(byte)
        result = (result * 16777619) & 0xffffffff
    return result


def uid_bytes(value):
    if isinstance(value, (bytes, bytearray)):
        raw = bytes(value)
        return raw[:6] + (b"\x00" * max(0, 6 - len(raw)))
    text = str(value or "").replace(":", "").replace("-", "").replace(" ", "")
    if len(text) >= 12:
        try:
            return bytes(int(text[idx:idx + 2], 16) for idx in range(0, 12, 2))
        except Exception:
            pass
    return b"\x00\x00\x00\x00\x00\x00"


def hex_uid(value):
    raw = uid_bytes(value)
    return "".join("{:02X}".format(byte) for byte in raw)


def crc16(data):
    crc = 0xffff
    for byte in data:
        crc ^= int(byte) << 8
        for _bit in range(8):
            if crc & 0x8000:
                crc = ((crc << 1) ^ 0x1021) & 0xffff
            else:
                crc = (crc << 1) & 0xffff
    return crc & 0xffff


def _request_id(payload):
    if isinstance(payload, dict):
        return payload.get("request_id", "")
    return ""


def _require(data, offset, length):
    if offset + length > len(data):
        raise NHCPError("truncated_value")


def _pack_i64(value):
    value = int(value)
    if value < 0:
        value = (1 << 64) + value
    low = value & 0xffffffff
    high = (value >> 32) & 0xffffffff
    return struct.pack("<II", low, high)


def _unpack_i64(data, offset):
    low, high = struct.unpack_from("<II", data, offset)
    value = (int(high) << 32) | int(low)
    if value & (1 << 63):
        value -= 1 << 64
    return value
