#!/usr/bin/env python3
import argparse
import json
import socket
import struct
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


class HostUiService:
    def __init__(self, http_host="0.0.0.0", http_port=8787, udp_port=5005, control_local_port=22345):
        self.http_host = http_host
        self.http_port = int(http_port)
        self.udp_port = int(udp_port)
        self.registry = DeviceRegistry()
        self.control = ControlClient(control_local_port)
        self.stop_event = threading.Event()
        self.httpd = None
        self.udp_thread = threading.Thread(target=self._udp_loop, daemon=True)
        self.status_thread = threading.Thread(target=self._status_loop, daemon=True)

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
            self.registry.upsert_packet(addr, packet)
        sock.close()

    def _status_loop(self):
        while not self.stop_event.is_set():
            hosts = self.registry.all_hosts()
            for host, port in hosts:
                try:
                    response = self.control.send_command(host, port, {"command": "status"}, timeout=1.0)
                except OSError:
                    continue
                if response.get("status") == "ok":
                    self.registry.apply_status(host, response, port=port)
            self.stop_event.wait(5.0)

    def start(self):
        self.udp_thread.start()
        self.status_thread.start()
        self.httpd = ThreadingHTTPServer((self.http_host, self.http_port), self._make_handler())
        print("Host UI listening on http://{}:{}".format(self.http_host, self.http_port))
        print("UDP data receiver on 0.0.0.0:{}".format(self.udp_port))
        self.httpd.serve_forever()

    def stop(self):
        self.stop_event.set()
        if self.httpd is not None:
            self.httpd.shutdown()

    def _make_handler(self):
        service = self

        class Handler(BaseHTTPRequestHandler):
            def do_GET(self):
                parsed = urlparse(self.path)
                if parsed.path == "/":
                    return self._html()
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
                        service.registry.apply_status(record["host"], service.control.send_command(record["host"], record.get("port", 22345), {"command": "status"}, timeout=1.0), port=record.get("port", 22345))
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

        return Handler


INDEX_HTML = """<!doctype html>
<html lang="zh-Hant">
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>New Horizons Host Console</title>
  <style>
    :root {
      --bg: #08131a;
      --panel: rgba(14, 27, 34, 0.88);
      --panel-2: rgba(7, 17, 22, 0.84);
      --line: rgba(127, 182, 199, 0.18);
      --ink: #d7e9ed;
      --muted: #7fa6b2;
      --accent: #4fd1c5;
      --accent-2: #f6b73c;
      --danger: #ff6b6b;
      --good: #5be584;
      --shadow: 0 20px 70px rgba(0, 0, 0, 0.4);
    }
    * { box-sizing: border-box; }
    body {
      margin: 0;
      min-height: 100vh;
      color: var(--ink);
      font-family: "Avenir Next", "Trebuchet MS", sans-serif;
      background:
        radial-gradient(circle at top left, rgba(79, 209, 197, 0.18), transparent 34%),
        radial-gradient(circle at bottom right, rgba(246, 183, 60, 0.16), transparent 28%),
        linear-gradient(135deg, #050d11 0%, #0a1c24 44%, #071117 100%);
    }
    .shell {
      display: grid;
      grid-template-columns: 280px minmax(0, 1fr) 360px;
      gap: 18px;
      padding: 18px;
    }
    .panel {
      background: var(--panel);
      border: 1px solid var(--line);
      border-radius: 24px;
      box-shadow: var(--shadow);
      backdrop-filter: blur(18px);
      overflow: hidden;
    }
    .panel h2, .panel h3 {
      margin: 0;
      font-family: "Iowan Old Style", "Palatino Linotype", serif;
      letter-spacing: 0.03em;
    }
    .panel-head {
      padding: 18px 20px 12px;
      border-bottom: 1px solid var(--line);
      display: flex;
      justify-content: space-between;
      align-items: end;
      gap: 12px;
    }
    .panel-body { padding: 16px 20px 20px; }
    .tag {
      display: inline-block;
      padding: 4px 10px;
      border-radius: 999px;
      background: rgba(79, 209, 197, 0.12);
      color: var(--accent);
      font-size: 12px;
      letter-spacing: 0.06em;
      text-transform: uppercase;
    }
    .device-list { display: grid; gap: 10px; }
    .device-card {
      border: 1px solid var(--line);
      border-radius: 18px;
      padding: 14px;
      background: var(--panel-2);
      cursor: pointer;
      transition: transform 140ms ease, border-color 140ms ease, background 140ms ease;
    }
    .device-card:hover { transform: translateY(-1px); border-color: rgba(79, 209, 197, 0.35); }
    .device-card.active { border-color: rgba(246, 183, 60, 0.5); background: rgba(246, 183, 60, 0.08); }
    .label { color: var(--muted); font-size: 12px; text-transform: uppercase; letter-spacing: 0.08em; }
    .value { font-size: 14px; }
    .heat-wrap { display: grid; gap: 14px; }
    .stats {
      display: grid;
      grid-template-columns: repeat(4, minmax(0, 1fr));
      gap: 10px;
    }
    .stat {
      border: 1px solid var(--line);
      border-radius: 18px;
      padding: 12px;
      background: rgba(255, 255, 255, 0.02);
    }
    .stat .big { font-size: 22px; margin-top: 6px; }
    .grid {
      display: grid;
      gap: 5px;
      padding: 14px;
      border-radius: 22px;
      background: rgba(3, 10, 14, 0.9);
      border: 1px solid var(--line);
      min-height: 320px;
    }
    .cell {
      aspect-ratio: 1 / 1;
      border-radius: 10px;
      border: 1px solid rgba(255,255,255,0.06);
      display: flex;
      align-items: center;
      justify-content: center;
      font-size: 11px;
      color: rgba(255,255,255,0.78);
      font-family: "SF Mono", "Menlo", monospace;
      transition: transform 80ms ease;
    }
    .cell:hover { transform: scale(1.06); }
    .section { display: grid; gap: 12px; margin-bottom: 18px; }
    .section:last-child { margin-bottom: 0; }
    .button-row, .inline-row { display: flex; gap: 8px; flex-wrap: wrap; }
    button, input, select {
      border-radius: 14px;
      border: 1px solid var(--line);
      background: rgba(255,255,255,0.04);
      color: var(--ink);
      padding: 10px 12px;
      font: inherit;
    }
    button {
      cursor: pointer;
      background: linear-gradient(180deg, rgba(79, 209, 197, 0.18), rgba(79, 209, 197, 0.08));
    }
    button.warn { background: linear-gradient(180deg, rgba(246, 183, 60, 0.22), rgba(246, 183, 60, 0.08)); }
    button.danger { background: linear-gradient(180deg, rgba(255, 107, 107, 0.2), rgba(255, 107, 107, 0.08)); }
    input, select { width: 100%; }
    .form-grid {
      display: grid;
      grid-template-columns: repeat(2, minmax(0, 1fr));
      gap: 10px;
    }
    pre.log {
      margin: 0;
      max-height: 220px;
      overflow: auto;
      padding: 14px;
      border-radius: 18px;
      background: rgba(0, 0, 0, 0.32);
      border: 1px solid var(--line);
      color: #b7dce3;
      font-family: "SF Mono", "Menlo", monospace;
      font-size: 12px;
      white-space: pre-wrap;
    }
    .small { font-size: 12px; color: var(--muted); }
    @media (max-width: 1180px) {
      .shell { grid-template-columns: 1fr; }
      .stats { grid-template-columns: repeat(2, minmax(0, 1fr)); }
    }
  </style>
</head>
<body>
  <div class="shell">
    <section class="panel">
      <div class="panel-head">
        <div>
          <div class="tag">Device Rail</div>
          <h2>Known Boards</h2>
        </div>
      </div>
      <div class="panel-body">
        <div class="section">
          <div class="form-grid">
            <input id="hostInput" placeholder="192.168.1.152">
            <input id="portInput" placeholder="22345" value="22345">
          </div>
          <button id="addDeviceBtn">Add Device Host</button>
        </div>
        <div id="deviceList" class="device-list"></div>
      </div>
    </section>

    <section class="panel">
      <div class="panel-head">
        <div>
          <div class="tag">Live Matrix</div>
          <h2 id="titleText">No Device Selected</h2>
        </div>
        <div class="small" id="subtitleText">Waiting for packets</div>
      </div>
      <div class="panel-body heat-wrap">
        <div class="stats">
          <div class="stat"><div class="label">Min</div><div class="big" id="minVal">-</div></div>
          <div class="stat"><div class="label">Max</div><div class="big" id="maxVal">-</div></div>
          <div class="stat"><div class="label">Avg</div><div class="big" id="avgVal">-</div></div>
          <div class="stat"><div class="label">Frame</div><div class="big" id="frameVal">-</div></div>
        </div>
        <div id="heatmap" class="grid"></div>
        <div class="section">
          <div class="label">Calibration Level Preview</div>
          <div id="calibrationMap" class="grid"></div>
        </div>
      </div>
    </section>

    <section class="panel">
      <div class="panel-head">
        <div>
          <div class="tag">Control Surface</div>
          <h2>Board Ops</h2>
        </div>
      </div>
      <div class="panel-body">
        <div class="section">
          <div class="label">Quick Actions</div>
          <div class="button-row">
            <button data-cmd="status">Status</button>
            <button data-cmd="check_update">Check Update</button>
            <button data-cmd="apply_update">Apply Update</button>
            <button data-cmd="upgrade_to_full" class="warn">Upgrade To Full</button>
            <button data-cmd="reboot" class="danger">Reboot</button>
          </div>
        </div>

        <div class="section">
          <div class="label">Server Binding</div>
          <div class="form-grid">
            <input id="masterHost" placeholder="master host">
            <input id="masterPort" placeholder="master port">
            <input id="dataHost" placeholder="data host">
            <input id="dataPort" placeholder="data port">
          </div>
          <button id="saveServersBtn">Save Servers</button>
        </div>

        <div class="section">
          <div class="label">Calibration Mode</div>
          <div class="button-row">
            <button id="enterCalBtn">Enter Calibration</button>
            <button id="exitCalBtn" class="warn">Exit Calibration</button>
          </div>
          <div class="form-grid">
            <select id="analogPin"></select>
            <select id="selectPin"></select>
            <input id="calLevel" placeholder="level" value="0.000">
            <input id="calDelay" placeholder="start delay ms" value="1000">
            <input id="calDuration" placeholder="duration ms" value="5000">
          </div>
          <div class="button-row">
            <button id="singleCalBtn">Calibrate Single Cell</button>
            <button id="fullCalBtn" class="warn">Calibrate Full Matrix</button>
          </div>
          <div class="inline-row">
            <select id="levelSelect"></select>
            <button id="loadLevelBtn">Load Level Matrix</button>
            <button id="deleteLevelBtn" class="danger">Delete Level</button>
          </div>
        </div>

        <div class="section">
          <div class="label">Last Response</div>
          <pre id="responseLog" class="log">Host UI ready.</pre>
        </div>
      </div>
    </section>
  </div>

  <script>
    const state = { devices: [], selectedKey: null, selectedDevice: null };

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
      if (value === null || Number.isNaN(value)) return "rgba(255,255,255,0.05)";
      const span = Math.max(max - min, 1e-6);
      const ratio = Math.min(1, Math.max(0, (value - min) / span));
      const hue = 210 - ratio * 180;
      const light = 16 + ratio * 46;
      return `hsl(${hue} 82% ${light}%)`;
    }

    function renderGrid(targetId, values, rows, cols) {
      const el = document.getElementById(targetId);
      el.innerHTML = "";
      if (!values || !rows || !cols) {
        el.textContent = "No matrix data";
        el.style.gridTemplateColumns = "1fr";
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

    function renderDeviceList() {
      const list = document.getElementById("deviceList");
      list.innerHTML = "";
      state.devices.forEach(device => {
        const item = document.createElement("div");
        item.className = "device-card" + (device.key === state.selectedKey ? " active" : "");
        item.innerHTML = `
          <div class="label">${device.device_uid || device.device_id || device.key}</div>
          <div class="value">${device.device_name || device.host}</div>
          <div class="small">${device.host}:${device.port} ${device.channel || ""}</div>
        `;
        item.onclick = () => {
          state.selectedKey = device.key;
          fetchSelected();
          renderDeviceList();
        };
        list.appendChild(item);
      });
    }

    function syncPinSelectors(device) {
      const rows = device?.status?.active_rows || [];
      const cols = device?.status?.active_cols || [];
      const rowSelect = document.getElementById("analogPin");
      const colSelect = document.getElementById("selectPin");
      rowSelect.innerHTML = rows.map(v => `<option value="${v}">${v}</option>`).join("");
      colSelect.innerHTML = cols.map(v => `<option value="${v}">${v}</option>`).join("");
      const levels = device?.status?.calibration_levels || device?.levels || [];
      document.getElementById("levelSelect").innerHTML = levels.map(v => `<option value="${v}">${v}</option>`).join("");
    }

    function renderSelected() {
      const device = state.selectedDevice;
      if (!device) return;
      document.getElementById("titleText").textContent = device.device_name || device.host;
      document.getElementById("subtitleText").textContent = `${device.host}:${device.port}  ${device.device_uid || device.device_id || ""}`;

      const packet = device.packet || {};
      const matrix = packet.matrix || [];
      const rows = packet.rows || device.status?.matrix_shape?.rows || 0;
      const cols = packet.cols || device.status?.matrix_shape?.cols || 0;
      const numeric = matrix.filter(v => typeof v === "number");
      const min = numeric.length ? Math.min(...numeric) : null;
      const max = numeric.length ? Math.max(...numeric) : null;
      const avg = numeric.length ? numeric.reduce((a, b) => a + b, 0) / numeric.length : null;
      document.getElementById("minVal").textContent = min === null ? "-" : min.toFixed(2);
      document.getElementById("maxVal").textContent = max === null ? "-" : max.toFixed(2);
      document.getElementById("avgVal").textContent = avg === null ? "-" : avg.toFixed(2);
      document.getElementById("frameVal").textContent = packet.frame_id ?? "-";
      renderGrid("heatmap", matrix, rows, cols);
      syncPinSelectors(device);

      const runtime = device.status?.runtime || {};
      document.getElementById("masterHost").value = runtime.master_server?.host || "";
      document.getElementById("masterPort").value = runtime.master_server?.port || 22345;
      document.getElementById("dataHost").value = runtime.data_server?.host || "";
      document.getElementById("dataPort").value = runtime.data_server?.port || 5005;
    }

    async function fetchDevices() {
      const data = await api("/api/devices");
      state.devices = data.devices || [];
      if (!state.selectedKey && state.devices.length) {
        state.selectedKey = state.devices[0].key;
      }
      renderDeviceList();
    }

    async function fetchSelected() {
      if (!state.selectedKey) return;
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

    async function loop() {
      await fetchDevices();
      await fetchSelected();
      setTimeout(loop, 800);
    }

    loop();
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
