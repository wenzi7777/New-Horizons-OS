# packet.py
import struct
import sys

import config
import secrets
from crypto_hmac import hmac_sha256
from device_identity import get_device_id, get_device_name

HEADER_LEN = 18


class PacketBuilder:
    def __init__(self):
        self.hmac_len = config.HMAC_LEN if config.USE_HMAC else 0
        self.os_version = self._get_os_version()
        self.device_id = get_device_id()
        self.device_name = get_device_name(config.DEVICE_NAME)

        self.active_rows = list(config.ACTIVE_ROWS)
        self.active_cols = list(config.ACTIVE_COLS)

        total = len(self.active_rows) * len(self.active_cols)

        if config.ACTIVE_SENSOR_COUNT is None:
            self.matrix_count = total
        else:
            self.matrix_count = min(config.ACTIVE_SENSOR_COUNT, total)

    def build(self, frame_id, timestamp_ms, matrix, imu=None, battery=None):
        if getattr(config, "PACKET_FORMAT", "BINARY") == "TEXT_LINE":
            return self._build_text_line(
                frame_id=frame_id,
                timestamp_ms=timestamp_ms,
                matrix=matrix,
                imu=imu,
                battery=battery
            )

        return self._build_binary(
            frame_id=frame_id,
            timestamp_ms=timestamp_ms,
            matrix=matrix,
            imu=imu,
            battery=battery
        )

    def _get_os_version(self):
        try:
            name = sys.implementation.name
        except Exception:
            name = "MicroPython"

        try:
            version = sys.version.split(" ")[0]
        except Exception:
            version = "unknown"

        return "{}-{}".format(name, version)

    def _build_text_line(self, frame_id, timestamp_ms, matrix, imu=None, battery=None):
        ax, ay, az, gx, gy, gz, chip_temp = self._normalize_imu_text(imu)

        fields = [
            self.device_name,
            "0x%08X" % self.device_id,
            self.os_version,
            str(timestamp_ms),
            self._fmt_float(ax, config.BMI270_ACC_DECIMALS),
            self._fmt_float(ay, config.BMI270_ACC_DECIMALS),
            self._fmt_float(az, config.BMI270_ACC_DECIMALS),
            self._fmt_float(gx, config.BMI270_GYRO_DECIMALS),
            self._fmt_float(gy, config.BMI270_GYRO_DECIMALS),
            self._fmt_float(gz, config.BMI270_GYRO_DECIMALS),
            self._fmt_temp(chip_temp),
        ]

        self._append_matrix_text_fields(fields, matrix)

        if getattr(config, "TEXT_LINE_HMAC", False):
            body = ",".join(fields).encode()
            tag = hmac_sha256(secrets.HMAC_KEY, body)[:config.HMAC_LEN]
            fields.append("HMAC|" + tag.hex())

        fields.append(config.TEXT_END_MARKER)

        return (",".join(fields) + "\n").encode()

    def _fmt_float(self, value, digits):
        try:
            return ("%." + str(digits) + "f") % float(value)
        except Exception:
            return "0"

    def _fmt_temp(self, value):
        if value == "NA" or value is None:
            return "NA"

        try:
            return ("%." + str(config.BMI270_TEMP_DECIMALS) + "f") % float(value)
        except Exception:
            return "NA"

    def _normalize_imu_text(self, imu):
        if imu is None:
            return 0, 0, 0, 0, 0, 0, "NA"

        try:
            if len(imu) >= 7:
                return imu[0], imu[1], imu[2], imu[3], imu[4], imu[5], imu[6]

            if len(imu) >= 6:
                return imu[0], imu[1], imu[2], imu[3], imu[4], imu[5], "NA"

        except Exception:
            pass

        return 0, 0, 0, 0, 0, 0, "NA"

    def _append_matrix_text_fields(self, fields, matrix):
        with_label = getattr(config, "TEXT_MATRIX_WITH_LABEL", True)

        ncols = len(self.active_cols)
        count = min(len(matrix), self.matrix_count)

        for i in range(count):
            row_index = i // ncols
            col_index = i % ncols

            if row_index >= len(self.active_rows):
                return

            row = self.active_rows[row_index]
            col = self.active_cols[col_index]
            value = int(matrix[i])

            if with_label:
                fields.append("R{}C{}|{}".format(row, col, value))
            else:
                fields.append(str(value))

    def _build_binary(self, frame_id, timestamp_ms, matrix, imu=None, battery=None):
        sensor_count = len(matrix)
        matrix_payload_len = sensor_count * 4

        imu_payload = self._pack_imu_binary(imu)
        battery_payload = self._pack_battery_binary(battery)

        payload_len = matrix_payload_len + len(imu_payload) + len(battery_payload)

        flags = 0
        if imu_payload:
            flags |= 0x01
        if battery_payload:
            flags |= 0x02
        if config.USE_HMAC:
            flags |= 0x80

        total_len = HEADER_LEN + payload_len + self.hmac_len
        buf = bytearray(total_len)

        struct.pack_into(
            "<HBBIIIH",
            buf,
            0,
            config.MAGIC,
            config.PACKET_VERSION,
            flags,
            self.device_id,
            frame_id,
            timestamp_ms,
            payload_len
        )

        offset = HEADER_LEN

        for value in matrix:
            struct.pack_into("<f", buf, offset, float(value))
            offset += 4

        if imu_payload:
            buf[offset:offset + len(imu_payload)] = imu_payload
            offset += len(imu_payload)

        if battery_payload:
            buf[offset:offset + len(battery_payload)] = battery_payload
            offset += len(battery_payload)

        if config.USE_HMAC:
            body_len = HEADER_LEN + payload_len
            tag = hmac_sha256(secrets.HMAC_KEY, buf[:body_len])[:self.hmac_len]
            buf[body_len:body_len + self.hmac_len] = tag

        return buf

    def _pack_imu_binary(self, imu):
        if imu is None:
            return b""

        try:
            ax, ay, az, gx, gy, gz = imu[:6]
            payload = [float(ax), float(ay), float(az), float(gx), float(gy), float(gz)]
            if len(imu) >= 7 and imu[6] != "NA":
                payload.append(float(imu[6]))
            else:
                payload.append(0.0)
            return struct.pack("<" + ("f" * len(payload)), *payload)

        except Exception:
            return b""

    def _pack_battery_binary(self, battery):
        if battery is None:
            return b""

        try:
            status, fault, vbat_mv = battery
            return struct.pack(
                "<BBH",
                int(status) & 0xFF,
                int(fault) & 0xFF,
                int(vbat_mv) & 0xFFFF
            )
        except Exception:
            return b""
