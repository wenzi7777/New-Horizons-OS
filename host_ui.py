#!/usr/bin/env python3
import argparse
import json
import queue
import socket
import struct
import sys
import threading
import time
from datetime import datetime, timezone
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import unquote, urlparse


MAGIC = 0xA55A
HEADER_LEN = 18
HMAC_LEN = 16
FLAG_IMU = 0x01
FLAG_BATTERY = 0x02
FLAG_HMAC = 0x80


def utc_iso_from_ms(timestamp_ms):
    return datetime.fromtimestamp(timestamp_ms / 1000.0, tz=timezone.utc).isoformat()


def now_ms():
    return int(time.time() * 1000)


def infer_shape(matrix_count):
    if matrix_count == 210:
        return 10, 21
    if matrix_count == 200:
        return 10, 20
    if matrix_count == 60:
        return 6, 10
    if matrix_count == 50:
        return 5, 10
    if matrix_count <= 0:
        return 0, 0
    for cols in (21, 20, 16, 15, 12, 10, 8, 6, 5, 4, 3, 2):
        if matrix_count % cols == 0:
            return matrix_count // cols, cols
    return 1, matrix_count


def parse_binary_packet(data):
    if len(data) < HEADER_LEN:
        raise ValueError("packet too short")

    magic, version, flags, device_id_int, frame_id, timestamp_ms, payload_len = struct.unpack_from(
        "<HBBIIIH",
        data,
        0,
    )
    if magic != MAGIC:
        raise ValueError("bad magic")

    has_hmac = (flags & FLAG_HMAC) != 0
    expected_len = HEADER_LEN + payload_len + (HMAC_LEN if has_hmac else 0)
    if len(data) != expected_len:
        raise ValueError("bad packet length")

    body = data[:-HMAC_LEN] if has_hmac else data
    payload = body[HEADER_LEN:]

    tail_len = 0
    if flags & FLAG_IMU:
        tail_len += 28
    if flags & FLAG_BATTERY:
        tail_len += 4
    if payload_len < tail_len:
        raise ValueError("payload too short")

    matrix_bytes = payload_len - tail_len
    if matrix_bytes % 4 != 0:
        raise ValueError("matrix payload not float32 aligned")

    matrix_count = matrix_bytes // 4
    offset = 0
    matrix = list(struct.unpack_from("<" + ("f" * matrix_count), payload, offset))
    offset += matrix_bytes

    imu = None
    if flags & FLAG_IMU:
        ax, ay, az, gx, gy, gz, chip_temp = struct.unpack_from("<fffffff", payload, offset)
        offset += 28
        imu = {
            "ax": ax,
            "ay": ay,
            "az": az,
            "gx": gx,
            "gy": gy,
            "gz": gz,
            "chip_temp": chip_temp,
        }

    battery = None
    if flags & FLAG_BATTERY:
        status, fault, vbat_mv = struct.unpack_from("<BBH", payload, offset)
        battery = {
            "status": status,
            "fault": fault,
            "vbat_mv": vbat_mv,
        }

    rows, cols = infer_shape(matrix_count)
    return {
        "device_id": "0x{:08X}".format(device_id_int),
        "frame_id": frame_id,
        "timestamp_ms": timestamp_ms,
        "payload_len": payload_len,
        "rows": rows,
        "cols": cols,
        "matrix": matrix,
        "imu": imu,
        "battery": battery,
        "pc_time_iso": datetime.now(tz=timezone.utc).isoformat(),
        "packet_version": version,
        "flags": flags,
    }


class DeviceRegistry:
    def __init__(self):
        self.lock = threading.Lock()
        self.devices = {}

    def _find_key(self, device_uid="", device_id="", host=""):
        for key, record in self.devices.items():
            if device_uid and record.get("device_uid") == device_uid:
                return key
        for key, record in self.devices.items():
            if device_id and record.get("device_id") == device_id:
                return key
        for key, record in self.devices.items():
            if host and record.get("host") == host:
                return key
        return None

    def register_host(self, host, port=22345):
        with self.lock:
            key = self._find_key(host=host) or "host:{}".format(host)
            record = self.devices.setdefault(key, {"key": key})
            record.setdefault("device_id", "")
            record.setdefault("device_uid", "")
            record.setdefault("device_name", host)
            record["host"] = host
            record["port"] = int(port)
            record["last_seen_ms"] = record.get("last_seen_ms", 0)
            return dict(record)

    def upsert_packet(self, addr, packet):
        host = addr[0]
        with self.lock:
            key = self._find_key(device_id=packet.get("device_id", ""), host=host)
            if key is None:
                key = packet.get("device_id") or "host:{}".format(host)
            record = self.devices.setdefault(key, {"key": key, "port": 22345})
            previous_packet = record.get("packet", {})
            previous_frame_id = previous_packet.get("frame_id")
            previous_timestamp_ms = previous_packet.get("timestamp_ms")
            current_frame_id = packet.get("frame_id")
            current_timestamp_ms = packet.get("timestamp_ms")
            if all(value is not None for value in (
                previous_frame_id,
                previous_timestamp_ms,
                current_frame_id,
                current_timestamp_ms,
            )):
                frame_delta = current_frame_id - previous_frame_id
                elapsed_ms = current_timestamp_ms - previous_timestamp_ms
                if frame_delta > 0 and elapsed_ms > 0:
                    record["scan_fps"] = frame_delta * 1000.0 / elapsed_ms
            record["host"] = host
            record["data_port"] = int(addr[1])
            record["device_id"] = packet.get("device_id", record.get("device_id", ""))
            if packet.get("device_name"):
                record["device_name"] = packet["device_name"]
            record["packet"] = packet
            record["last_seen_ms"] = now_ms()
            record["last_seen_iso"] = datetime.now(tz=timezone.utc).isoformat()
            return dict(record)

    def apply_status(self, host, status, port=22345):
        device_uid = status.get("device_uid", "")
        device_id = status.get("device_id", "")
        with self.lock:
            old_key = self._find_key(device_uid=device_uid, device_id=device_id, host=host)
            new_key = device_uid or device_id or old_key or "host:{}".format(host)
            if old_key is None:
                record = {"key": new_key}
            else:
                record = self.devices.pop(old_key)
            record["key"] = new_key
            record["host"] = host
            record["port"] = int(port)
            record["device_id"] = device_id or record.get("device_id", "")
            record["device_uid"] = device_uid or record.get("device_uid", "")
            record["device_name"] = status.get("device_name", record.get("device_name", host))
            record["status"] = status
            record["last_status_ms"] = now_ms()
            if "last_seen_ms" not in record:
                record["last_seen_ms"] = 0
            self.devices[new_key] = record
            return dict(record)

    def get(self, key):
        with self.lock:
            record = self.devices.get(key)
            return json.loads(json.dumps(record)) if record is not None else None

    def list_devices(self):
        with self.lock:
            items = []
            for key, record in self.devices.items():
                packet = record.get("packet", {})
                status = record.get("status", {})
                items.append({
                    "key": key,
                    "host": record.get("host", ""),
                    "port": record.get("port", 22345),
                    "device_id": record.get("device_id", ""),
                    "device_uid": record.get("device_uid", ""),
                    "device_name": record.get("device_name", record.get("host", "")),
                    "last_seen_ms": record.get("last_seen_ms", 0),
                    "last_status_ms": record.get("last_status_ms", 0),
                    "frame_id": packet.get("frame_id"),
                    "rows": packet.get("rows"),
                    "cols": packet.get("cols"),
                    "scan_fps": record.get("scan_fps"),
                    "wifi_state": status.get("wifi_state"),
                    "channel": status.get("runtime", {}).get("channel") if status else None,
                })
            items.sort(key=lambda item: (item.get("last_seen_ms", 0), item.get("last_status_ms", 0)), reverse=True)
            return items

    def all_hosts(self):
        with self.lock:
            hosts = []
            for record in self.devices.values():
                host = record.get("host")
                if host:
                    hosts.append((host, int(record.get("port", 22345))))
            return hosts


class DeviceEventHub:
    def __init__(self):
        self.lock = threading.Lock()
        self.subscribers = set()

    def subscribe(self):
        subscriber = queue.Queue(maxsize=1)
        with self.lock:
            self.subscribers.add(subscriber)
        return subscriber

    def unsubscribe(self, subscriber):
        with self.lock:
            self.subscribers.discard(subscriber)

    def publish(self, payload):
        with self.lock:
            subscribers = list(self.subscribers)
        for subscriber in subscribers:
            try:
                subscriber.put_nowait(payload)
                continue
            except queue.Full:
                pass
            try:
                subscriber.get_nowait()
            except queue.Empty:
                pass
            try:
                subscriber.put_nowait(payload)
            except queue.Full:
                pass


class QuietThreadingHTTPServer(ThreadingHTTPServer):
    def handle_error(self, request, client_address):
        error = sys.exc_info()[1]
        if isinstance(error, (BrokenPipeError, ConnectionResetError)):
            return
        return super().handle_error(request, client_address)


class ControlClient:
    def __init__(self, local_port=22345):
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.sock.bind(("0.0.0.0", int(local_port)))
        self.sock.settimeout(2.0)
        self.lock = threading.Lock()

    def send_command(self, host, port, payload, timeout=2.0):
        data = json.dumps(payload).encode()
        with self.lock:
            self.sock.sendto(data, (host, int(port)))
            deadline = time.time() + timeout
            while time.time() < deadline:
                try:
                    resp, addr = self.sock.recvfrom(8192)
                except socket.timeout:
                    break
                if addr[0] != host:
                    continue
                try:
                    return json.loads(resp.decode())
                except Exception:
                    return {"status": "ok", "message": "raw_response", "raw": resp.decode(errors="replace")}
        return {"status": "error", "message": "timeout", "error": "timeout"}

    def poll_announcements(self, handler, timeout=0.2, max_packets=32):
        with self.lock:
            previous_timeout = self.sock.gettimeout()
            try:
                self.sock.settimeout(timeout)
                processed = 0
                while processed < max_packets:
                    try:
                        payload, addr = self.sock.recvfrom(8192)
                    except socket.timeout:
                        break
                    processed += 1
                    try:
                        message = json.loads(payload.decode())
                    except Exception:
                        continue
                    handler(addr, message)
            finally:
                self.sock.settimeout(previous_timeout)


class HostUiService:
    def __init__(self, http_host="0.0.0.0", http_port=8787, udp_port=5005, control_local_port=22345):
        self.http_host = http_host
        self.http_port = int(http_port)
        self.udp_port = int(udp_port)
        self.registry = DeviceRegistry()
        self.events = DeviceEventHub()
        self.control = ControlClient(control_local_port)
        self.stop_event = threading.Event()
        self.httpd = None
        self.udp_thread = threading.Thread(target=self._udp_loop, daemon=True)
        self.control_thread = threading.Thread(target=self._control_loop, daemon=True)

    def _ingest_control_announcements(self):
        def handle(addr, message):
            if message.get("status") == "ok" and message.get("device_id"):
                record = self.registry.apply_status(addr[0], message, port=addr[1])
                self._publish_record(record, kind="status")

        try:
            self.control.poll_announcements(handle, timeout=0.2)
        except OSError:
            return

    def _udp_loop(self):
        sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        sock.bind(("0.0.0.0", self.udp_port))
        sock.settimeout(0.5)
        while not self.stop_event.is_set():
            try:
                data, addr = sock.recvfrom(65535)
            except socket.timeout:
                continue
            except OSError:
                break
            try:
                packet = parse_binary_packet(data)
            except Exception:
                continue
            record = self.registry.upsert_packet(addr, packet)
            self._publish_record(record, kind="packet")
        sock.close()

    def _control_loop(self):
        while not self.stop_event.is_set():
            self._ingest_control_announcements()

    def start(self):
        self.udp_thread.start()
        self.control_thread.start()
        self.httpd = QuietThreadingHTTPServer((self.http_host, self.http_port), self._make_handler())
        print("Host UI listening on http://{}:{}".format(self.http_host, self.http_port))
        print("UDP data receiver on 0.0.0.0:{}".format(self.udp_port))
        print("UDP control receiver on 0.0.0.0:{}".format(self.control.sock.getsockname()[1]))
        self.httpd.serve_forever()

    def stop(self):
        self.stop_event.set()
        if self.httpd is not None:
            self.httpd.shutdown()

    def _publish_record(self, record, kind="status"):
        if not record:
            return
        latest = self.registry.get(record.get("key", "")) or record
        self.events.publish({"kind": kind, "device": latest})

    def _make_handler(self):
        service = self

        class Handler(BaseHTTPRequestHandler):
            def do_GET(self):
                parsed = urlparse(self.path)
                if parsed.path == "/":
                    return self._html()
                if parsed.path == "/api/stream":
                    return self._event_stream()
                if parsed.path == "/api/devices":
                    return self._json({"devices": service.registry.list_devices()})
                if parsed.path.startswith("/api/devices/"):
                    key = unquote(parsed.path[len("/api/devices/"):])
                    record = service.registry.get(key)
                    if record is None:
                        return self._json({"status": "error", "message": "device_not_found"}, status=404)
                    return self._json(record)
                return self._json({"status": "error", "message": "not_found"}, status=404)

            def do_POST(self):
                parsed = urlparse(self.path)
                body = self._read_json()
                if body is None:
                    return self._json({"status": "error", "message": "invalid_json"}, status=400)

                if parsed.path == "/api/devices":
                    host = body.get("host", "").strip()
                    port = int(body.get("port", 22345))
                    if not host:
                        return self._json({"status": "error", "message": "host_required"}, status=400)
                    record = service.registry.register_host(host, port)
                    return self._json({"status": "ok", "device": record})

                if parsed.path.startswith("/api/devices/") and parsed.path.endswith("/command"):
                    key = unquote(parsed.path[len("/api/devices/"):-len("/command")])
                    record = service.registry.get(key)
                    if record is None:
                        return self._json({"status": "error", "message": "device_not_found"}, status=404)
                    response = service.control.send_command(record["host"], record.get("port", 22345), body, timeout=4.0)
                    if response.get("status") == "ok":
                        if response.get("device_id"):
                            updated = service.registry.apply_status(record["host"], response, port=record.get("port", 22345))
                            service._publish_record(updated, kind="status")
                    return self._json({"status": "ok", "response": response, "device": service.registry.get(key) or service.registry.get(response.get("device_uid", ""))})

                return self._json({"status": "error", "message": "not_found"}, status=404)

            def log_message(self, _format, *_args):
                return

            def _read_json(self):
                try:
                    length = int(self.headers.get("Content-Length", "0"))
                except ValueError:
                    return None
                data = self.rfile.read(length or 0)
                try:
                    return json.loads(data.decode() or "{}")
                except Exception:
                    return None

            def _json(self, payload, status=200):
                encoded = json.dumps(payload).encode()
                self.send_response(status)
                self.send_header("Content-Type", "application/json")
                self.send_header("Content-Length", str(len(encoded)))
                self.end_headers()
                self.wfile.write(encoded)

            def _html(self):
                encoded = INDEX_HTML.encode()
                self.send_response(200)
                self.send_header("Content-Type", "text/html; charset=utf-8")
                self.send_header("Content-Length", str(len(encoded)))
                self.end_headers()
                self.wfile.write(encoded)

            def _event_stream(self):
                subscriber = service.events.subscribe()
                self.send_response(200)
                self.send_header("Content-Type", "text/event-stream")
                self.send_header("Cache-Control", "no-cache")
                self.send_header("Connection", "keep-alive")
                self.send_header("X-Accel-Buffering", "no")
                self.end_headers()
                try:
                    while not service.stop_event.is_set():
                        try:
                            payload = subscriber.get(timeout=15.0)
                            encoded = json.dumps(payload, separators=(",", ":")).encode()
                            self.wfile.write(b"event: device\n")
                            self.wfile.write(b"data: " + encoded + b"\n\n")
                        except queue.Empty:
                            self.wfile.write(b": keep-alive\n\n")
                        self.wfile.flush()
                except (BrokenPipeError, ConnectionResetError, OSError):
                    return
                finally:
                    service.events.unsubscribe(subscriber)

        return Handler


INDEX_HTML = """<!doctype html>
<html lang="zh-Hant">
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>New Horizons Host Console</title>
  <style>
    :root {
      --bg: #070909;
      --panel: #0f1416;
      --panel-2: #12191c;
      --panel-3: #181f23;
      --line: rgba(176, 191, 199, 0.14);
      --line-strong: rgba(208, 220, 226, 0.24);
      --text: #e5ecee;
      --muted: #8d9ba2;
      --soft: #aebcc2;
      --accent: #cfd8dc;
      --accent-hot: #ffb347;
      --danger: #d96a58;
      --success: #8bc2ae;
      --shadow: 0 18px 40px rgba(0, 0, 0, 0.34);
    }
    * { box-sizing: border-box; }
    html, body { min-height: 100%; }
    body {
      margin: 0;
      color: var(--text);
      font-family: "IBM Plex Sans", "Avenir Next", "Helvetica Neue", sans-serif;
      background:
        linear-gradient(180deg, rgba(255,255,255,0.02), transparent 18%),
        radial-gradient(circle at top right, rgba(255, 179, 71, 0.09), transparent 22%),
        linear-gradient(180deg, #050606 0%, #090c0d 100%);
    }
    body::before {
      content: "";
      position: fixed;
      inset: 0;
      pointer-events: none;
      opacity: 0.09;
      background-image:
        linear-gradient(rgba(255,255,255,0.05) 1px, transparent 1px),
        linear-gradient(90deg, rgba(255,255,255,0.04) 1px, transparent 1px);
      background-size: 28px 28px;
      mask-image: linear-gradient(180deg, rgba(0,0,0,0.95), rgba(0,0,0,0.5));
    }
    .app {
      position: relative;
      padding: 18px;
      display: grid;
      gap: 18px;
    }
    .topbar {
      display: flex;
      align-items: end;
      justify-content: space-between;
      gap: 16px;
      padding: 18px 22px;
      border: 1px solid var(--line);
      background: linear-gradient(180deg, rgba(255,255,255,0.035), rgba(255,255,255,0.015));
      box-shadow: var(--shadow);
    }
    .topbar h1 {
      margin: 0;
      font-size: clamp(26px, 4vw, 42px);
      font-family: "Avenir Next Condensed", "Arial Narrow", sans-serif;
      font-weight: 700;
      letter-spacing: 0.12em;
      text-transform: uppercase;
    }
    .eyebrow,
    .section-kicker,
    .metric-label,
    .field-label,
    .response-label {
      font-family: "IBM Plex Mono", "SF Mono", "Menlo", monospace;
      letter-spacing: 0.12em;
      text-transform: uppercase;
      color: var(--muted);
      font-size: 11px;
    }
    .topbar-meta {
      display: flex;
      flex-wrap: wrap;
      gap: 10px;
      justify-content: flex-end;
    }
    .status-pill {
      display: inline-flex;
      align-items: center;
      gap: 8px;
      min-height: 34px;
      padding: 0 12px;
      border: 1px solid var(--line);
      background: rgba(255,255,255,0.03);
      color: var(--soft);
      font-size: 12px;
    }
    .status-pill::before {
      content: "";
      width: 7px;
      height: 7px;
      border-radius: 50%;
      background: var(--muted);
    }
    .status-pill.live::before { background: var(--success); }
    .status-pill.alert::before { background: var(--accent-hot); }
    .layout {
      display: grid;
      grid-template-columns: 320px minmax(0, 1fr) 400px;
      gap: 18px;
      align-items: start;
    }
    .stack {
      display: grid;
      gap: 18px;
      min-width: 0;
    }
    .panel {
      border: 1px solid var(--line);
      background:
        linear-gradient(180deg, rgba(255,255,255,0.025), rgba(255,255,255,0.008)),
        var(--panel);
      box-shadow: var(--shadow);
      min-width: 0;
    }
    .panel-head {
      display: flex;
      justify-content: space-between;
      gap: 16px;
      align-items: start;
      padding: 18px 20px 14px;
      border-bottom: 1px solid var(--line);
    }
    .panel-title {
      display: grid;
      gap: 6px;
    }
    .panel-title h2,
    .panel-title h3 {
      margin: 0;
      font-size: 20px;
      line-height: 1.1;
      font-weight: 650;
      letter-spacing: 0.02em;
    }
    .panel-body {
      padding: 18px 20px 20px;
      display: grid;
      gap: 18px;
      min-width: 0;
    }
    .input-row,
    .dual-grid,
    .calibration-grid,
    .server-grid {
      display: grid;
      gap: 10px;
    }
    .input-row,
    .dual-grid,
    .server-grid {
      grid-template-columns: repeat(2, minmax(0, 1fr));
    }
    .calibration-grid {
      grid-template-columns: repeat(2, minmax(0, 1fr));
    }
    .pin-layout-grid {
      display: grid;
      gap: 12px;
    }
    .pin-grid {
      display: grid;
      grid-template-columns: repeat(5, minmax(0, 1fr));
      gap: 8px;
    }
    .pin-chip {
      min-height: 34px;
      border: 1px solid var(--line);
      background: rgba(255,255,255,0.02);
      display: flex;
      align-items: center;
      gap: 8px;
      padding: 0 10px;
      font-family: "IBM Plex Mono", "SF Mono", "Menlo", monospace;
      font-size: 12px;
      color: var(--soft);
    }
    .pin-chip input {
      width: auto;
      margin: 0;
      accent-color: var(--accent-hot);
    }
    .device-list {
      display: grid;
      gap: 10px;
      max-height: 68vh;
      overflow: auto;
      padding-right: 2px;
    }
    .device-card {
      border: 1px solid var(--line);
      background: linear-gradient(180deg, rgba(255,255,255,0.022), rgba(255,255,255,0.006));
      padding: 14px;
      display: grid;
      gap: 10px;
      cursor: pointer;
      transition: border-color 120ms ease, transform 120ms ease, background 120ms ease;
    }
    .device-card:hover {
      border-color: var(--line-strong);
      transform: translateY(-1px);
      background: linear-gradient(180deg, rgba(255,255,255,0.04), rgba(255,255,255,0.01));
    }
    .device-card.active {
      border-color: rgba(255, 179, 71, 0.62);
      background: linear-gradient(180deg, rgba(255, 179, 71, 0.12), rgba(255,255,255,0.01));
    }
    .device-card-head,
    .device-card-meta,
    .overview-strip,
    .button-grid,
    .button-row,
    .inline-row,
    .status-row {
      display: flex;
      gap: 8px;
      flex-wrap: wrap;
    }
    .device-name {
      font-size: 15px;
      font-weight: 600;
      color: var(--text);
    }
    .device-key,
    .device-host,
    .mono {
      font-family: "IBM Plex Mono", "SF Mono", "Menlo", monospace;
      font-size: 12px;
      color: var(--soft);
      overflow-wrap: anywhere;
    }
    .mini-pill {
      display: inline-flex;
      align-items: center;
      min-height: 24px;
      padding: 0 9px;
      border: 1px solid var(--line);
      background: rgba(255,255,255,0.028);
      color: var(--soft);
      font-size: 11px;
      text-transform: uppercase;
      letter-spacing: 0.08em;
    }
    .overview-strip { align-items: center; }
    .hero-metrics {
      display: grid;
      grid-template-columns: repeat(6, minmax(0, 1fr));
      gap: 10px;
    }
    .metric,
    .status-card,
    .control-block,
    .response-shell {
      border: 1px solid var(--line);
      background: var(--panel-2);
    }
    .metric {
      padding: 12px 14px;
      display: grid;
      gap: 8px;
    }
    .metric-value {
      font-size: clamp(22px, 3vw, 28px);
      line-height: 1;
      font-weight: 650;
      color: var(--accent);
    }
    .status-grid {
      display: grid;
      grid-template-columns: repeat(2, minmax(0, 1fr));
      gap: 10px;
    }
    .status-card {
      padding: 12px 14px;
      display: grid;
      gap: 10px;
    }
    .status-value {
      font-size: 18px;
      font-weight: 600;
      word-break: break-word;
    }
    .status-text {
      font-size: 12px;
      color: var(--soft);
      min-height: 16px;
    }
    .meter {
      width: 100%;
      height: 10px;
      border: 1px solid var(--line);
      background: #0a0e10;
      overflow: hidden;
    }
    .meter-fill {
      width: 0%;
      height: 100%;
      background: linear-gradient(90deg, #9aadb5 0%, #ffb347 100%);
      transition: width 140ms ease;
    }
    .canvas-shell {
      border: 1px solid var(--line);
      background: linear-gradient(180deg, rgba(255,255,255,0.02), rgba(255,255,255,0));
      padding: 14px;
      display: grid;
      gap: 14px;
    }
    .grid {
      display: grid;
      gap: 5px;
      background: #06090a;
      border: 1px solid rgba(255,255,255,0.06);
      padding: 10px;
      min-height: 280px;
      align-content: start;
    }
    .grid.calibration-grid-view {
      min-height: 200px;
    }
    .grid-empty {
      display: flex;
      align-items: center;
      justify-content: center;
      color: var(--muted);
      font-family: "IBM Plex Mono", "SF Mono", "Menlo", monospace;
      font-size: 12px;
      letter-spacing: 0.06em;
      text-transform: uppercase;
      min-height: 180px;
    }
    .cell {
      aspect-ratio: 1 / 1;
      display: flex;
      align-items: center;
      justify-content: center;
      border: 1px solid rgba(255,255,255,0.08);
      color: #eef5f6;
      font-family: "IBM Plex Mono", "SF Mono", "Menlo", monospace;
      font-size: 11px;
      transition: transform 80ms ease;
    }
    .cell:hover { transform: scale(1.04); }
    .control-block {
      padding: 14px;
      display: grid;
      gap: 12px;
    }
    .control-heading {
      display: grid;
      gap: 4px;
    }
    .control-title {
      margin: 0;
      font-size: 14px;
      font-weight: 650;
      letter-spacing: 0.05em;
      text-transform: uppercase;
    }
    .control-note {
      font-size: 12px;
      color: var(--muted);
      line-height: 1.5;
    }
    .button-grid {
      display: grid;
      grid-template-columns: repeat(2, minmax(0, 1fr));
      gap: 10px;
    }
    button,
    input,
    select {
      width: 100%;
      min-height: 42px;
      border: 1px solid var(--line);
      background: #101619;
      color: var(--text);
      padding: 10px 12px;
      font: inherit;
      outline: none;
    }
    button {
      cursor: pointer;
      font-weight: 600;
      letter-spacing: 0.04em;
      text-transform: uppercase;
      font-size: 12px;
      background:
        linear-gradient(180deg, rgba(255,255,255,0.05), rgba(255,255,255,0.015)),
        #131a1d;
    }
    button:hover {
      border-color: var(--line-strong);
      background:
        linear-gradient(180deg, rgba(255,255,255,0.08), rgba(255,255,255,0.02)),
        #171f23;
    }
    button.warn {
      color: #17120a;
      border-color: rgba(255, 179, 71, 0.28);
      background:
        linear-gradient(180deg, rgba(255, 179, 71, 0.95), rgba(205, 135, 42, 0.95));
    }
    button.danger {
      color: #f5d6d1;
      border-color: rgba(217, 106, 88, 0.32);
      background:
        linear-gradient(180deg, rgba(123, 53, 41, 0.95), rgba(82, 33, 26, 0.95));
    }
    input::placeholder { color: #6f7b81; }
    .response-shell {
      padding: 14px;
      display: grid;
      gap: 10px;
    }
    pre.log {
      margin: 0;
      max-height: 220px;
      overflow: auto;
      padding: 12px;
      border: 1px solid rgba(255,255,255,0.06);
      background: #090c0d;
      color: #bdd1d8;
      font-family: "IBM Plex Mono", "SF Mono", "Menlo", monospace;
      font-size: 12px;
      line-height: 1.55;
      white-space: pre-wrap;
    }
    .empty-card {
      border: 1px dashed var(--line);
      padding: 18px 16px;
      color: var(--muted);
      font-size: 13px;
      line-height: 1.6;
      background: rgba(255,255,255,0.01);
    }
    @media (max-width: 1320px) {
      .layout {
        grid-template-columns: 280px minmax(0, 1fr);
      }
      .stack-right {
        grid-column: 1 / -1;
      }
    }
    @media (max-width: 960px) {
      .app { padding: 12px; }
      .layout { grid-template-columns: 1fr; }
      .hero-metrics,
      .status-grid,
      .button-grid,
      .input-row,
      .dual-grid,
      .server-grid,
      .calibration-grid {
        grid-template-columns: 1fr;
      }
      .pin-grid {
        grid-template-columns: repeat(3, minmax(0, 1fr));
      }
      .topbar {
        flex-direction: column;
        align-items: start;
      }
      .topbar-meta {
        justify-content: start;
      }
    }
  </style>
</head>
<body>
  <div class="app">
    <header class="topbar">
      <div>
        <div class="eyebrow">Industrial Control Surface</div>
        <h1>Host Console</h1>
      </div>
      <div class="topbar-meta">
        <div id="topDeviceCount" class="status-pill">0 devices</div>
        <div id="topSelectionState" class="status-pill">No active board</div>
        <div class="status-pill alert">UDP 5005 / CTRL 22345</div>
      </div>
    </header>

    <main class="layout">
      <section class="panel">
        <div class="panel-head">
          <div class="panel-title">
            <div class="section-kicker">Fleet Rail</div>
            <h2>Devices</h2>
          </div>
          <div id="deviceRailState" class="mini-pill">idle</div>
        </div>
        <div class="panel-body">
          <div class="control-block">
            <div class="control-heading">
              <div class="control-title">Add Target</div>
              <div class="control-note">Register a board by host address when passive discovery has not populated it yet.</div>
            </div>
            <div class="input-row">
              <input id="hostInput" placeholder="192.168.1.152">
              <input id="portInput" placeholder="22345" value="22345">
            </div>
            <button id="addDeviceBtn">Add Device Host</button>
          </div>
          <div id="deviceList" class="device-list"></div>
        </div>
      </section>

      <div class="stack">
        <section class="panel">
          <div class="panel-head">
            <div class="panel-title">
              <div class="section-kicker">Board Overview</div>
              <h2 id="titleText">No Device Selected</h2>
            </div>
            <div id="subtitleText" class="mono">Waiting for packets</div>
          </div>
          <div class="panel-body">
            <div class="overview-strip">
              <div id="overviewChannel" class="mini-pill">channel -</div>
              <div id="overviewWifi" class="mini-pill">wifi -</div>
              <div id="overviewOta" class="mini-pill">ota idle</div>
            </div>
            <div class="hero-metrics">
              <div class="metric">
                <div class="metric-label">Min</div>
                <div class="metric-value" id="minVal">-</div>
              </div>
              <div class="metric">
                <div class="metric-label">Max</div>
                <div class="metric-value" id="maxVal">-</div>
              </div>
              <div class="metric">
                <div class="metric-label">Avg</div>
                <div class="metric-value" id="avgVal">-</div>
              </div>
              <div class="metric">
                <div class="metric-label">Frame</div>
                <div class="metric-value" id="frameVal">-</div>
              </div>
              <div class="metric">
                <div class="metric-label">Scan FPS</div>
                <div class="metric-value" id="scanFpsVal">-</div>
              </div>
              <div class="metric">
                <div class="metric-label">UI FPS</div>
                <div class="metric-value" id="uiFpsVal">-</div>
              </div>
            </div>
            <div class="status-grid">
              <div class="status-card">
                <div class="metric-label">Ota Phase</div>
                <div class="status-value" id="otaPhaseVal">idle</div>
                <div class="status-text" id="otaProgressText">No OTA activity</div>
                <div class="meter"><div id="otaProgressFill" class="meter-fill"></div></div>
              </div>
              <div class="status-card">
                <div class="metric-label">Ota Detail</div>
                <div class="status-value" id="otaFilesVal">-</div>
                <div class="status-text" id="otaCurrentVal">-</div>
                <div class="status-text" id="otaResultVal">-</div>
              </div>
            </div>
          </div>
        </section>

        <section class="panel">
          <div class="panel-head">
            <div class="panel-title">
              <div class="section-kicker">Live Pressure Matrix</div>
              <h2>Sensor View</h2>
            </div>
          </div>
          <div class="panel-body">
            <div class="canvas-shell">
              <div id="heatmap" class="grid"></div>
            </div>
          </div>
        </section>

        <section class="panel">
          <div class="panel-head">
            <div class="panel-title">
              <div class="section-kicker">Calibration Snapshot</div>
              <h2>Level Preview</h2>
            </div>
          </div>
          <div class="panel-body">
            <div class="canvas-shell">
              <div id="calibrationMap" class="grid calibration-grid-view"></div>
            </div>
          </div>
        </section>
      </div>

      <div class="stack stack-right">
        <section class="panel">
          <div class="panel-head">
            <div class="panel-title">
              <div class="section-kicker">Primary Actions</div>
              <h2>Operations</h2>
            </div>
          </div>
          <div class="panel-body">
            <div class="control-block">
              <div class="control-heading">
                <div class="control-title">Quick Commands</div>
                <div class="control-note">Most-used device actions grouped for direct access.</div>
              </div>
              <div class="button-grid">
                <button data-cmd="status">Status</button>
                <button data-cmd="check_update">Check Update</button>
                <button data-cmd="apply_update">Apply Update</button>
                <button data-cmd="upgrade_to_full" class="warn">Upgrade To Full</button>
                <button data-cmd="reboot" class="danger">Reboot</button>
              </div>
            </div>

            <div class="control-block">
              <div class="control-heading">
                <div class="control-title">Server Binding</div>
                <div class="control-note">Edit master and data endpoints for the selected device.</div>
              </div>
              <div class="server-grid">
                <div>
                  <div class="field-label">Master Host</div>
                  <input id="masterHost" placeholder="master host">
                </div>
                <div>
                  <div class="field-label">Master Port</div>
                  <input id="masterPort" placeholder="master port">
                </div>
                <div>
                  <div class="field-label">Data Host</div>
                  <input id="dataHost" placeholder="data host">
                </div>
                <div>
                  <div class="field-label">Data Port</div>
                  <input id="dataPort" placeholder="data port">
                </div>
              </div>
              <button id="saveServersBtn">Save Servers</button>
            </div>

            <div class="control-block">
              <div class="control-heading">
                <div class="control-title">Matrix Layout</div>
                <div class="control-note">Select active row and column GPIOs. Empty layout keeps scanning disabled.</div>
              </div>
              <div class="pin-layout-grid">
                <div>
                  <div class="field-label">Active Row Pins</div>
                  <div id="matrixRows" class="pin-grid"></div>
                </div>
                <div>
                  <div class="field-label">Active Column Pins</div>
                  <div id="matrixCols" class="pin-grid"></div>
                </div>
              </div>
              <button id="saveMatrixBtn">Save Matrix Layout</button>
            </div>

            <div class="control-block">
              <div class="control-heading">
                <div class="control-title">Calibration</div>
                <div class="control-note">Enter calibration mode, sample cells, or capture the whole matrix baseline.</div>
              </div>
              <div class="button-row">
                <button id="enterCalBtn">Enter Calibration</button>
                <button id="exitCalBtn" class="warn">Exit Calibration</button>
              </div>
              <div class="calibration-grid">
                <div>
                  <div class="field-label">Analog Pin</div>
                  <select id="analogPin"></select>
                </div>
                <div>
                  <div class="field-label">Select Pin</div>
                  <select id="selectPin"></select>
                </div>
                <div>
                  <div class="field-label">Level</div>
                  <input id="calLevel" placeholder="level" value="0.000">
                </div>
                <div>
                  <div class="field-label">Start Delay Ms</div>
                  <input id="calDelay" placeholder="start delay ms" value="1000">
                </div>
                <div>
                  <div class="field-label">Duration Ms</div>
                  <input id="calDuration" placeholder="duration ms" value="5000">
                </div>
                <div>
                  <div class="field-label">Saved Level</div>
                  <select id="levelSelect"></select>
                </div>
              </div>
              <div class="button-grid">
                <button id="singleCalBtn">Calibrate Single Cell</button>
                <button id="fullCalBtn" class="warn">Calibrate Full Matrix</button>
                <button id="loadLevelBtn">Load Level Matrix</button>
                <button id="deleteLevelBtn" class="danger">Delete Level</button>
              </div>
            </div>

            <div class="response-shell">
              <div class="response-label">Last Response</div>
              <pre id="responseLog" class="log">Host UI ready.</pre>
            </div>
          </div>
        </section>
      </div>
    </main>
  </div>

  <script>
    const state = {
      devices: [],
      selectedKey: null,
      selectedDevice: null,
      matrixLayoutDeviceKey: null,
      matrixLayoutDirty: false,
      matrixLayoutDraft: null,
      uiFpsWindowStartedAt: null,
      uiFpsFrameCount: 0,
      uiFps: null,
      lastRenderedFrameId: null,
      pendingSelectedDevice: null,
      pendingSelectedKind: null,
      streamRenderScheduled: false,
      deviceStreamFallbackStarted: false
    };

    function log(message) {
      const text = typeof message === "string" ? message : JSON.stringify(message, null, 2);
      document.getElementById("responseLog").textContent = text;
    }

    async function api(path, options = {}) {
      const response = await fetch(path, {
        headers: { "Content-Type": "application/json" },
        ...options
      });
      return response.json();
    }

    function colorFor(value, min, max) {
      if (value === null || Number.isNaN(value)) return "rgba(255,255,255,0.04)";
      const span = Math.max(max - min, 1e-6);
      const ratio = Math.min(1, Math.max(0, (value - min) / span));
      const light = 14 + ratio * 58;
      const warmth = 210 - ratio * 170;
      return `hsl(${warmth} 48% ${light}%)`;
    }

    function setGridEmpty(targetId, message) {
      const el = document.getElementById(targetId);
      el.innerHTML = `<div class="grid-empty">${message}</div>`;
      el.style.gridTemplateColumns = "1fr";
    }

    function renderGrid(targetId, values, rows, cols) {
      const el = document.getElementById(targetId);
      el.innerHTML = "";
      if (!values || !rows || !cols) {
        setGridEmpty(targetId, "No matrix data");
        return;
      }
      el.style.gridTemplateColumns = `repeat(${cols}, minmax(0, 1fr))`;
      const filtered = values.filter(v => typeof v === "number" && !Number.isNaN(v));
      const min = filtered.length ? Math.min(...filtered) : 0;
      const max = filtered.length ? Math.max(...filtered) : 1;
      values.forEach((value, index) => {
        const cell = document.createElement("div");
        cell.className = "cell";
        cell.style.background = colorFor(value, min, max);
        cell.title = `#${index} ${value === null ? "null" : value.toFixed ? value.toFixed(3) : value}`;
        cell.textContent = value === null ? "-" : (value > 999 ? value.toFixed(0) : value.toFixed(1));
        el.appendChild(cell);
      });
    }

    function formatPhase(phase) {
      const map = {
        idle: "idle",
        checking_manifest: "checking",
        ready: "ready",
        downloading: "applying",
        done: "done",
        error: "error"
      };
      return map[phase] || phase || "idle";
    }

    function formatWifi(device) {
      const wifiState = device?.status?.wifi_state;
      if (wifiState) return wifiState;
      if (device?.status?.wifi_connected === true) return "connected";
      if (device?.status?.wifi_connected === false) return "offline";
      return "unknown";
    }

    function renderDeviceList() {
      const list = document.getElementById("deviceList");
      const count = state.devices.length;
      document.getElementById("topDeviceCount").textContent = `${count} device${count === 1 ? "" : "s"}`;
      document.getElementById("deviceRailState").textContent = count ? "online" : "empty";
      list.innerHTML = "";

      if (!count) {
        list.innerHTML = '<div class="empty-card">No devices registered yet. Wait for status announcements or add a board host manually.</div>';
        return;
      }

      state.devices.forEach(device => {
        const item = document.createElement("div");
        item.className = "device-card" + (device.key === state.selectedKey ? " active" : "");
        item.innerHTML = `
          <div class="device-card-head">
            <div class="device-name">${device.device_name || device.host}</div>
            <div class="mini-pill">${device.channel || "unknown"}</div>
          </div>
          <div class="device-key">${device.device_uid || device.device_id || device.key}</div>
          <div class="device-card-meta">
            <div class="mini-pill">${formatWifi(device)}</div>
            <div class="mini-pill">${device.host}:${device.port}</div>
          </div>
        `;
        item.onclick = () => {
          state.selectedKey = device.key;
          fetchSelected();
          renderDeviceList();
        };
        list.appendChild(item);
      });
    }

    function renderUpdateState(updateState) {
      const phase = updateState?.phase || "idle";
      const totalFiles = Number(updateState?.total_files || 0);
      const appliedFiles = Number(updateState?.applied_files || 0);
      const currentFile = updateState?.current_file || "-";
      const lastResult = updateState?.last_result || "-";
      const rebootRequired = Boolean(updateState?.reboot_required);
      const lastError = updateState?.last_error || "";
      const percent = totalFiles > 0
        ? Math.max(0, Math.min(100, Math.round((appliedFiles / totalFiles) * 100)))
        : (phase === "done" ? 100 : 0);

      document.getElementById("otaPhaseVal").textContent = formatPhase(phase);
      document.getElementById("otaFilesVal").textContent = totalFiles ? `${appliedFiles}/${totalFiles} files` : "-";
      document.getElementById("otaCurrentVal").textContent = currentFile;
      document.getElementById("otaResultVal").textContent = lastResult;
      document.getElementById("otaProgressFill").style.width = `${percent}%`;
      document.getElementById("overviewOta").textContent = `ota ${formatPhase(phase)}`;

      let text = "No OTA activity";
      if (phase === "checking_manifest") text = "Checking manifest and planning file diff.";
      else if (phase === "ready") text = totalFiles ? `Update ready. ${totalFiles} files pending.` : "Manifest checked. No files pending.";
      else if (phase === "downloading") text = totalFiles ? `Applying ${appliedFiles + 1}/${totalFiles}: ${currentFile}` : `Applying ${currentFile}`;
      else if (phase === "done") text = rebootRequired ? "Update complete. Device reboot is pending." : "Update completed.";
      else if (phase === "error") text = lastError ? `Update failed: ${lastError}` : "Update failed.";
      document.getElementById("otaProgressText").textContent = text;
    }

    function syncPinSelectors(device) {
      const rows = device?.status?.active_rows || [];
      const cols = device?.status?.active_cols || [];
      const availableRows = device?.status?.available_rows || [];
      const availableCols = device?.status?.available_cols || [];
      const rowSelect = document.getElementById("analogPin");
      const colSelect = document.getElementById("selectPin");
      rowSelect.innerHTML = rows.map(v => `<option value="${v}">${logicalPinName(v, availableRows, "A")}</option>`).join("");
      colSelect.innerHTML = cols.map(v => `<option value="${v}">${logicalPinName(v, availableCols, "D")}</option>`).join("");
      const levels = device?.status?.calibration_levels || device?.levels || [];
      document.getElementById("levelSelect").innerHTML = levels.map(v => `<option value="${v}">${v}</option>`).join("");
    }

    function logicalPinName(pin, availablePins, prefix) {
      const index = (availablePins || []).indexOf(pin);
      return index >= 0 ? `${prefix}${index}` : String(pin);
    }

    function renderPinGrid(targetId, availablePins, activePins) {
      const target = document.getElementById(targetId);
      const active = new Set(activePins || []);
      const labelPrefix = targetId === "matrixRows" ? "A" : "D";
      target.innerHTML = (availablePins || []).map((pin, index) => `
        <label class="pin-chip">
          <input type="checkbox" value="${pin}" ${active.has(pin) ? "checked" : ""}>
          <span>${logicalPinName(pin, availablePins, labelPrefix)}</span>
        </label>
      `).join("");
      if (targetId === "matrixRows" || targetId === "matrixCols") {
        target.onchange = captureMatrixLayoutDraft;
      }
    }

    function matrixLayoutFromStatus(status) {
      return {
        active_rows: [...(status?.active_rows || [])],
        active_cols: [...(status?.active_cols || [])]
      };
    }

    function resetMatrixLayoutDraft(device) {
      state.matrixLayoutDeviceKey = device?.key || null;
      state.matrixLayoutDirty = false;
      state.matrixLayoutDraft = matrixLayoutFromStatus(device?.status || {});
    }

    function captureMatrixLayoutDraft() {
      if (!state.selectedDevice) return;
      state.matrixLayoutDeviceKey = state.selectedDevice.key || null;
      state.matrixLayoutDirty = true;
      state.matrixLayoutDraft = {
        active_rows: selectedPins("matrixRows"),
        active_cols: selectedPins("matrixCols")
      };
    }

    function syncMatrixLayout(device) {
      const status = device?.status || {};
      if (state.matrixLayoutDeviceKey !== (device?.key || null) || !state.matrixLayoutDraft) {
        resetMatrixLayoutDraft(device);
      } else if (!state.matrixLayoutDirty) {
        state.matrixLayoutDraft = matrixLayoutFromStatus(status);
      }
      renderPinGrid("matrixRows", status.available_rows || [], state.matrixLayoutDraft.active_rows);
      renderPinGrid("matrixCols", status.available_cols || [], state.matrixLayoutDraft.active_cols);
    }

    function selectedPins(targetId) {
      return Array.from(document.querySelectorAll(`#${targetId} input:checked`)).map(input => parseInt(input.value, 10));
    }

    function trackUiFps(packet) {
      const frameId = packet?.frame_id;
      if (frameId == null || frameId === state.lastRenderedFrameId) return;

      state.lastRenderedFrameId = frameId;
      const now = Date.now();
      if (state.uiFpsWindowStartedAt === null) {
        state.uiFpsWindowStartedAt = now;
      }
      state.uiFpsFrameCount += 1;

      const elapsed = now - state.uiFpsWindowStartedAt;
      if (elapsed >= 1000) {
        state.uiFps = state.uiFpsFrameCount * 1000 / Math.max(elapsed, 1);
        state.uiFpsWindowStartedAt = now;
        state.uiFpsFrameCount = 0;
      }
    }

    function flushSelectedRender() {
      state.streamRenderScheduled = false;
      if (!state.pendingSelectedDevice) return;
      const device = state.pendingSelectedDevice;
      const kind = state.pendingSelectedKind;
      state.selectedDevice = device;
      state.pendingSelectedDevice = null;
      state.pendingSelectedKind = null;
      if (kind === "packet") {
        renderSelectedFrame(device);
        return;
      }
      renderSelected();
    }

    function scheduleSelectedRender(device, kind) {
      state.pendingSelectedDevice = device;
      if (state.pendingSelectedKind !== "status") {
        state.pendingSelectedKind = kind || "status";
      }
      if (state.streamRenderScheduled) return;
      state.streamRenderScheduled = true;
      requestAnimationFrame(flushSelectedRender);
    }

    function applyDeviceEvent(payload) {
      const device = payload?.device;
      const kind = payload?.kind || "status";
      if (!device) return;

      const index = state.devices.findIndex(item => item.key === device.key);
      if (index >= 0) {
        state.devices[index] = { ...state.devices[index], ...device };
      }
      if (device.key === state.selectedKey) {
        scheduleSelectedRender(device, kind);
      }
    }

    function connectDeviceStream() {
      if (typeof EventSource !== "function") {
        startSelectedDeviceFallbackLoop();
        return;
      }
      const stream = new EventSource("/api/stream");
      stream.addEventListener("device", event => {
        try {
          applyDeviceEvent(JSON.parse(event.data));
        } catch (_err) {}
      });
    }

    function renderSelectedFrame(device) {
      const runtime = device.status?.runtime || {};
      const packet = device.packet || {};
      const matrixConfigured = device.status?.matrix_configured !== false;
      const activeRows = device.status?.active_rows || [];
      const activeCols = device.status?.active_cols || [];
      const rows = matrixConfigured ? activeRows.length : 0;
      const cols = matrixConfigured ? activeCols.length : 0;
      const expectedCells = rows * cols;
      const packetMatrix = matrixConfigured ? (packet.matrix || []) : [];
      const matrix = expectedCells
        ? Array.from({ length: expectedCells }, (_, index) => packetMatrix[index] ?? null)
        : [];
      const numeric = matrix.filter(v => typeof v === "number");
      const min = numeric.length ? Math.min(...numeric) : null;
      const max = numeric.length ? Math.max(...numeric) : null;
      const avg = numeric.length ? numeric.reduce((a, b) => a + b, 0) / numeric.length : null;

      trackUiFps(packet);
      document.getElementById("overviewChannel").textContent = `channel ${device.channel || runtime.channel || "-"}`;
      document.getElementById("overviewWifi").textContent = `wifi ${formatWifi(device)}`;
      document.getElementById("minVal").textContent = min === null ? "-" : min.toFixed(2);
      document.getElementById("maxVal").textContent = max === null ? "-" : max.toFixed(2);
      document.getElementById("avgVal").textContent = avg === null ? "-" : avg.toFixed(2);
      document.getElementById("frameVal").textContent = packet.frame_id ?? "-";
      document.getElementById("scanFpsVal").textContent = device.scan_fps == null ? "-" : device.scan_fps.toFixed(1);
      document.getElementById("uiFpsVal").textContent = state.uiFps == null ? "-" : state.uiFps.toFixed(1);
      renderGrid("heatmap", matrix, rows, cols);
    }

    function renderSelected() {
      const device = state.selectedDevice;
      if (!device) {
        state.matrixLayoutDeviceKey = null;
        state.matrixLayoutDirty = false;
        state.matrixLayoutDraft = null;
        state.uiFpsWindowStartedAt = null;
        state.uiFpsFrameCount = 0;
        state.uiFps = null;
        state.lastRenderedFrameId = null;
        document.getElementById("titleText").textContent = "No Device Selected";
        document.getElementById("subtitleText").textContent = "Waiting for packets";
        document.getElementById("topSelectionState").textContent = "No active board";
        document.getElementById("overviewChannel").textContent = "channel -";
        document.getElementById("overviewWifi").textContent = "wifi -";
        document.getElementById("overviewOta").textContent = "ota idle";
        ["minVal", "maxVal", "avgVal", "frameVal", "scanFpsVal", "uiFpsVal"].forEach(id => { document.getElementById(id).textContent = "-"; });
        renderUpdateState({});
        setGridEmpty("heatmap", "No matrix data");
        setGridEmpty("calibrationMap", "No calibration data");
        renderPinGrid("matrixRows", [], []);
        renderPinGrid("matrixCols", [], []);
        return;
      }

      document.getElementById("titleText").textContent = device.device_name || device.host;
      document.getElementById("subtitleText").textContent = `${device.host}:${device.port}  ${device.device_uid || device.device_id || ""}`;
      document.getElementById("topSelectionState").textContent = `Selected ${device.device_name || device.host}`;

      const runtime = device.status?.runtime || {};
      renderSelectedFrame(device);
      syncPinSelectors(device);
      syncMatrixLayout(device);
      renderUpdateState(device.status?.update_state || {});

      document.getElementById("masterHost").value = runtime.master_server?.host || "";
      document.getElementById("masterPort").value = runtime.master_server?.port || 22345;
      document.getElementById("dataHost").value = runtime.data_server?.host || "";
      document.getElementById("dataPort").value = runtime.data_server?.port || 5005;
    }

    async function fetchDevices() {
      const previousSelectedKey = state.selectedKey;
      const data = await api("/api/devices");
      state.devices = data.devices || [];
      if (!state.selectedKey && state.devices.length) {
        state.selectedKey = state.devices[0].key;
      }
      if (state.selectedKey && !state.devices.find(device => device.key === state.selectedKey)) {
        state.selectedKey = state.devices[0]?.key || null;
      }
      renderDeviceList();
      if (state.selectedKey !== previousSelectedKey) {
        await fetchSelected();
      }
    }

    async function fetchSelected() {
      if (!state.selectedKey) {
        state.selectedDevice = null;
        renderSelected();
        return;
      }
      const data = await api(`/api/devices/${encodeURIComponent(state.selectedKey)}`);
      state.selectedDevice = data;
      renderSelected();
    }

    async function sendCommand(payload) {
      if (!state.selectedKey) {
        log("No device selected");
        return;
      }
      const data = await api(`/api/devices/${encodeURIComponent(state.selectedKey)}/command`, {
        method: "POST",
        body: JSON.stringify(payload)
      });
      if (data.device) state.selectedDevice = data.device;
      log(data.response || data);
      await fetchDevices();
      await fetchSelected();
      return data;
    }

    document.getElementById("addDeviceBtn").onclick = async () => {
      const host = document.getElementById("hostInput").value.trim();
      const port = parseInt(document.getElementById("portInput").value.trim() || "22345", 10);
      const data = await api("/api/devices", { method: "POST", body: JSON.stringify({ host, port }) });
      log(data);
      await fetchDevices();
      if (data.device?.key) {
        state.selectedKey = data.device.key;
        await sendCommand({ command: "status" });
      }
    };

    document.querySelectorAll("[data-cmd]").forEach(button => {
      button.onclick = () => sendCommand({ command: button.dataset.cmd });
    });

    document.getElementById("saveServersBtn").onclick = () => sendCommand({
      command: "set_servers",
      master_server: {
        host: document.getElementById("masterHost").value.trim(),
        port: parseInt(document.getElementById("masterPort").value.trim() || "22345", 10)
      },
      data_server: {
        host: document.getElementById("dataHost").value.trim(),
        port: parseInt(document.getElementById("dataPort").value.trim() || "5005", 10)
      }
    });

    document.getElementById("saveMatrixBtn").onclick = async () => {
      const data = await sendCommand({
        command: "set_matrix_layout",
        active_rows: selectedPins("matrixRows"),
        active_cols: selectedPins("matrixCols")
      });
      if (data?.response?.status === "ok") {
        resetMatrixLayoutDraft(state.selectedDevice);
        syncMatrixLayout(state.selectedDevice);
      }
    };

    document.getElementById("enterCalBtn").onclick = () => sendCommand({ command: "enter_calibration_mode", enabled: true });
    document.getElementById("exitCalBtn").onclick = () => sendCommand({ command: "end_calibration" });
    document.getElementById("singleCalBtn").onclick = () => sendCommand({
      command: "start_calibration",
      analog_pin: parseInt(document.getElementById("analogPin").value, 10),
      select_pin: parseInt(document.getElementById("selectPin").value, 10),
      level: parseFloat(document.getElementById("calLevel").value),
      start_delay_ms: parseInt(document.getElementById("calDelay").value, 10),
      duration_ms: parseInt(document.getElementById("calDuration").value, 10)
    });
    document.getElementById("fullCalBtn").onclick = () => sendCommand({
      command: "calibrate_all",
      level: parseFloat(document.getElementById("calLevel").value),
      start_delay_ms: parseInt(document.getElementById("calDelay").value, 10),
      duration_ms: parseInt(document.getElementById("calDuration").value, 10)
    });
    document.getElementById("loadLevelBtn").onclick = async () => {
      const level = document.getElementById("levelSelect").value;
      await sendCommand({ command: "dump_calibration", level });
      const text = document.getElementById("responseLog").textContent;
      try {
        const parsed = JSON.parse(text);
        const matrix = parsed.data?.matrix || [];
        const rows = matrix.length;
        const cols = rows ? matrix[0].length : 0;
        renderGrid("calibrationMap", matrix.flat(), rows, cols);
      } catch (_err) {}
    };
    document.getElementById("deleteLevelBtn").onclick = () => sendCommand({
      command: "delete_calibration_level",
      level: document.getElementById("levelSelect").value
    });

    async function deviceListLoop() {
      await fetchDevices();
      setTimeout(deviceListLoop, 1000);
    }

    async function selectedDeviceFallbackLoop() {
      await fetchSelected();
      setTimeout(selectedDeviceFallbackLoop, 50);
    }

    function startSelectedDeviceFallbackLoop() {
      if (state.deviceStreamFallbackStarted) return;
      state.deviceStreamFallbackStarted = true;
      selectedDeviceFallbackLoop();
    }

    renderSelected();
    connectDeviceStream();
    deviceListLoop();
  </script>
</body>
</html>
"""


def main():
    parser = argparse.ArgumentParser(description="New Horizons OS local host UI")
    parser.add_argument("--http-host", default="0.0.0.0")
    parser.add_argument("--http-port", type=int, default=8787)
    parser.add_argument("--udp-port", type=int, default=5005)
    parser.add_argument("--control-local-port", type=int, default=22345)
    args = parser.parse_args()

    service = HostUiService(
        http_host=args.http_host,
        http_port=args.http_port,
        udp_port=args.udp_port,
        control_local_port=args.control_local_port,
    )
    try:
        service.start()
    except KeyboardInterrupt:
        pass
    finally:
        service.stop()


if __name__ == "__main__":
    main()
