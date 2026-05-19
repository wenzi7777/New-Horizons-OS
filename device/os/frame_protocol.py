import struct


PAYLOAD_TYPE_MV_U16 = 1
FRAME_HEADER_FORMAT = "<IIHHHH"
FRAME_HEADER_SIZE = struct.calcsize(FRAME_HEADER_FORMAT)


def encode_scan_frame(seq, timestamp_ms, rows, cols, payload_values, payload_type=PAYLOAD_TYPE_MV_U16):
    point_count = len(payload_values)
    header = struct.pack(
        FRAME_HEADER_FORMAT,
        int(seq),
        int(timestamp_ms),
        int(rows),
        int(cols),
        int(point_count),
        int(payload_type),
    )

    if payload_type != PAYLOAD_TYPE_MV_U16:
        raise ValueError("unsupported payload_type")

    payload = struct.pack("<" + ("H" * point_count), *[int(v) & 0xFFFF for v in payload_values])
    return header + payload


def decode_scan_frame(frame_bytes):
    if len(frame_bytes) < FRAME_HEADER_SIZE:
        raise ValueError("frame too short")

    seq, timestamp_ms, rows, cols, point_count, payload_type = struct.unpack_from(
        FRAME_HEADER_FORMAT,
        frame_bytes,
        0
    )

    if payload_type != PAYLOAD_TYPE_MV_U16:
        raise ValueError("unsupported payload_type")

    payload_len = point_count * 2
    expected_len = FRAME_HEADER_SIZE + payload_len
    if len(frame_bytes) < expected_len:
        raise ValueError("frame payload truncated")

    payload = struct.unpack_from("<" + ("H" * point_count), frame_bytes, FRAME_HEADER_SIZE)
    return {
        "seq": seq,
        "timestamp_ms": timestamp_ms,
        "rows": rows,
        "cols": cols,
        "point_count": point_count,
        "payload_type": payload_type,
        "payload_mv": payload,
    }
