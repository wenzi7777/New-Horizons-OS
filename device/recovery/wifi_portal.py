import socket


def _escape_html(text):
    text = str(text or "")
    text = text.replace("&", "&amp;")
    text = text.replace("<", "&lt;")
    text = text.replace(">", "&gt;")
    text = text.replace('"', "&quot;")
    return text


def _url_decode(text):
    text = str(text or "")
    out = []
    idx = 0
    while idx < len(text):
        ch = text[idx]
        if ch == "+":
            out.append(" ")
        elif ch == "%" and idx + 2 < len(text):
            try:
                out.append(chr(int(text[idx + 1:idx + 3], 16)))
                idx += 2
            except ValueError:
                out.append(ch)
        else:
            out.append(ch)
        idx += 1
    return "".join(out)


def _parse_form(body):
    result = {}
    for pair in str(body or "").split("&"):
        if not pair:
            continue
        if "=" in pair:
            key, value = pair.split("=", 1)
        else:
            key, value = pair, ""
        result[_url_decode(key)] = _url_decode(value)
    return result


def _normalize_path(path):
    path = str(path or "/")
    if path.startswith("http://") or path.startswith("https://"):
        scheme_idx = path.find("://")
        path_idx = path.find("/", scheme_idx + 3)
        path = path[path_idx:] if path_idx >= 0 else "/"
    if "?" in path:
        path = path.split("?", 1)[0]
    return path or "/"


CAPTIVE_PORTAL_PATHS = (
    "/generate_204",
    "/gen_204",
    "/hotspot-detect.html",
    "/library/test/success.html",
    "/connecttest.txt",
    "/ncsi.txt",
    "/success.txt",
    "/canonical.html",
    "/redirect",
    "/fwlink",
)

INDEX_CSS = (
    "*{box-sizing:border-box}"
    "body{font-family:Arial,sans-serif;margin:0;padding:16px;color:#111;background:#fff}"
    "main{max-width:680px;margin:auto}"
    "h1{font-size:24px;margin:8px 0}"
    "h2{font-size:16px;margin:18px 0 8px}"
    "p{line-height:1.35;margin:8px 0}"
    "label{display:block;margin:10px 0 4px;font-weight:700}"
    "input,select,button{width:100%;padding:10px;font-size:16px;border:1px solid #999;border-radius:6px}"
    "input[type=checkbox]{width:auto;margin:0 8px 0 0}"
    "button{margin-top:14px;background:#12325a;color:#fff;border:0}"
    ".msg{padding:10px;border:1px solid #bbb;background:#f6f6f6}"
    ".ok{color:#075b2a}.error{color:#9b1c14}"
    ".muted{color:#555;font-size:13px}"
    ".meta{border-top:1px solid #ddd;margin-top:14px;padding-top:8px;word-break:break-word}"
    ".check{display:flex;align-items:center}"
    ".overlay{display:none;position:fixed;inset:0;background:rgba(255,255,255,.92);z-index:9;align-items:center;justify-content:center;text-align:center;padding:24px}"
    ".overlay.on{display:flex}.spinner{width:38px;height:38px;border:4px solid #ccc;border-top-color:#12325a;border-radius:50%;animation:spin 1s linear infinite;margin:0 auto 16px}"
    "@keyframes spin{to{transform:rotate(360deg)}}"
)

RESULT_CSS = (
    "body{font-family:Arial,sans-serif;margin:0;padding:16px;color:#111}"
    "main{max-width:560px;margin:auto}"
    ".ok{color:#075b2a}.error{color:#9b1c14}"
    ".muted{color:#555}a{color:#12325a;font-weight:700}"
)


class CaptiveDnsServer:
    def __init__(self, ip_addr, logger=None):
        self.ip_addr = ip_addr or "192.168.4.1"
        self.logger = logger
        self.sock = None

    def start(self):
        if self.sock is not None:
            return True

        sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        try:
            sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        except Exception:
            pass
        sock.bind(("0.0.0.0", 53))
        try:
            sock.setblocking(False)
        except Exception:
            sock.settimeout(0)
        self.sock = sock
        return True

    def stop(self):
        if self.sock is not None:
            try:
                self.sock.close()
            except Exception:
                pass
        self.sock = None

    def service(self):
        if self.sock is None:
            return False

        try:
            data, addr = self.sock.recvfrom(512)
        except OSError:
            return False

        try:
            response = self._build_response(data)
            if response:
                self.sock.sendto(response, addr)
        except Exception as exc:
            if self.logger:
                self.logger.warn("wifi_captive_dns_failed {}".format(exc))
        return True

    def _build_response(self, data):
        if not data or len(data) < 12:
            return b""

        qdcount = (data[4] << 8) | data[5]
        if qdcount < 1:
            return b""

        idx = 12
        while idx < len(data):
            length = data[idx]
            idx += 1
            if length == 0:
                break
            idx += length

        if idx + 4 > len(data):
            return b""

        question_end = idx + 4
        question = data[12:question_end]
        ip_bytes = bytes(int(part) & 0xFF for part in self.ip_addr.split("."))
        header = bytearray(data[:2])
        header.extend(b"\x81\x80")
        header.extend(b"\x00\x01")
        header.extend(b"\x00\x01")
        header.extend(b"\x00\x00")
        header.extend(b"\x00\x00")
        answer = b"\xc0\x0c" + b"\x00\x01" + b"\x00\x01" + b"\x00\x00\x00\x1e" + b"\x00\x04" + ip_bytes
        return bytes(header) + question + answer


class WiFiSetupPortal:
    def __init__(self, manager, config_module, logger=None):
        self.manager = manager
        self.config = config_module
        self.logger = logger
        self.server = None
        self.dns = None
        self.active = False

    def start(self):
        if self.server is not None:
            self.active = True
            return True

        sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        try:
            sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        except Exception:
            pass
        sock.bind(("0.0.0.0", self.config.SETUP_PORTAL_PORT))
        sock.listen(1)
        try:
            sock.setblocking(False)
        except Exception:
            sock.settimeout(0)
        self.server = sock
        portal_ip = self.manager.portal_status().get("portal_ip", self.config.SETUP_PORTAL_HOST)
        try:
            self.dns = CaptiveDnsServer(portal_ip, self.logger)
            self.dns.start()
        except Exception as exc:
            self.dns = None
            if self.logger:
                self.logger.warn("wifi_captive_dns_start_failed {}".format(exc))
        self.active = True
        return True

    def stop(self):
        if self.dns is not None:
            self.dns.stop()
        self.dns = None
        if self.server is not None:
            try:
                self.server.close()
            except Exception:
                pass
        self.server = None
        self.active = False

    def service(self):
        if not self.active or self.server is None:
            return False

        dns_handled = False
        if self.dns is not None:
            dns_handled = self.dns.service()

        try:
            client, _addr = self.server.accept()
        except OSError:
            return dns_handled

        try:
            client.settimeout(1)
        except Exception:
            pass

        request_applied = False
        try:
            method, path, body = self._read_request(client)
            path = _normalize_path(path)
            if path == "/favicon.ico":
                self._send_response(client, "204 No Content", "text/plain", "")
                return True

            if path in CAPTIVE_PORTAL_PATHS:
                self._send_redirect(client, "/")
                return True

            if method == "POST" and path == "/connect":
                fields = _parse_form(body)
                result = self.manager.apply_credentials(
                    fields.get("ssid", ""),
                    fields.get("password", ""),
                    fields.get("server_profile", ""),
                    fields.get("mqtt_host", ""),
                    fields.get("mqtt_port", ""),
                    "",
                    "",
                    "",
                )
                request_applied = True
                content = self._render_result_page(result)
                self._send_response(client, "200 OK", "text/html; charset=utf-8", content)
                return True

            if path not in ("/", "/index.html"):
                self._send_redirect(client, "/")
                return True

            content = self._render_index_page()
            self._send_response(client, "200 OK", "text/html; charset=utf-8", content)
            return True
        except Exception as exc:
            if self.logger:
                self.logger.warn("wifi_portal_request_failed {}".format(exc))
            try:
                self._send_response(
                    client,
                    "500 Internal Server Error",
                    "text/plain; charset=utf-8",
                    "portal error",
                )
            except Exception:
                pass
            return request_applied
        finally:
            try:
                client.close()
            except Exception:
                pass

    def _read_request(self, client):
        data = b""
        while b"\r\n\r\n" not in data and len(data) < 8192:
            chunk = client.recv(1024)
            if not chunk:
                break
            data += chunk

        header_bytes, body = data, b""
        if b"\r\n\r\n" in data:
            header_bytes, body = data.split(b"\r\n\r\n", 1)

        header_text = header_bytes.decode("utf-8", "ignore")
        lines = header_text.split("\r\n")
        request_line = lines[0] if lines else "GET / HTTP/1.1"
        parts = request_line.split(" ")
        method = parts[0] if len(parts) > 0 else "GET"
        path = parts[1] if len(parts) > 1 else "/"

        content_length = 0
        for line in lines[1:]:
            if ":" not in line:
                continue
            name, value = line.split(":", 1)
            if name.strip().lower() == "content-length":
                try:
                    content_length = int(value.strip())
                except ValueError:
                    content_length = 0
                break

        while len(body) < content_length and len(body) < 8192:
            chunk = client.recv(min(1024, content_length - len(body)))
            if not chunk:
                break
            body += chunk

        return method.upper(), path, body.decode("utf-8", "ignore")

    def _send_response(self, client, status, content_type, body):
        body_bytes = body.encode("utf-8")
        header = (
            "HTTP/1.1 {status}\r\n"
            "Content-Type: {content_type}\r\n"
            "Content-Length: {length}\r\n"
            "Connection: close\r\n"
            "\r\n"
        ).format(
            status=status,
            content_type=content_type,
            length=len(body_bytes),
        )
        client.send(header.encode("utf-8"))
        client.send(body_bytes)

    def _send_redirect(self, client, location):
        header = (
            "HTTP/1.1 302 Found\r\n"
            "Location: {location}\r\n"
            "Content-Length: 0\r\n"
            "Connection: close\r\n"
            "\r\n"
        ).format(location=location)
        client.send(header.encode("utf-8"))

    def _render_index_page(self):
        status = self.manager.portal_status()
        networks = self.manager.scan_networks()
        selected_ssid = status.get("saved_ssid", "")
        selected_profile = status.get("server_profile", "")
        server_profile_options = status.get("server_profile_options", [])
        if not selected_profile:
            for item in server_profile_options:
                if item.get("value") == "production":
                    selected_profile = "production"
                    break

        options = ['<option value="">Manual input</option>']
        for item in networks:
            ssid = item.get("ssid", "")
            if not ssid:
                continue
            selected = " selected" if ssid == selected_ssid else ""
            label = "{} ({}, RSSI {})".format(
                ssid,
                item.get("security", "open"),
                item.get("rssi", 0),
            )
            options.append(
                '<option value="{value}"{selected}>{label}</option>'.format(
                    value=_escape_html(ssid),
                    selected=selected,
                    label=_escape_html(label),
                )
            )

        server_options = []
        manual_option = {}
        for item in server_profile_options:
            value = item.get("value", "")
            if not value:
                continue
            selected = " selected" if value == selected_profile else ""
            if value == "manual":
                manual_option = item
                label = item.get("label", value)
                developer_attr = ' data-developer="1"'
            else:
                label = "{} ({})".format(
                    item.get("label", value),
                    item.get("mqtt_host", ""),
                )
                developer_attr = ""
            server_options.append(
                '<option value="{value}"{selected}{developer_attr}>{label}</option>'.format(
                    value=_escape_html(value),
                    selected=selected,
                    developer_attr=developer_attr,
                    label=_escape_html(label),
                )
            )

        notice = ""
        if status.get("last_error"):
            notice = '<p class="msg error">Last error: {}</p>'.format(_escape_html(status["last_error"]))
        elif status.get("last_setup_result"):
            notice = '<p class="msg ok">Last result: {}</p>'.format(_escape_html(status["last_setup_result"]))

        ip_addr = _escape_html(status.get("portal_ip", self.config.SETUP_PORTAL_HOST))
        portal_url = _escape_html(status.get("portal_url", "http://{}".format(self.config.SETUP_PORTAL_HOST)))
        portal_ip_url = _escape_html(status.get("portal_ip_url", "http://{}".format(self.config.SETUP_PORTAL_HOST)))
        portal_domain = _escape_html(status.get("portal_domain", ""))
        ap_ssid = _escape_html(status.get("ap_ssid", self.config.SETUP_AP_SSID_PREFIX))
        mqtt_cfg = status.get("mqtt", {}) or {}
        recovery_mode = status.get("mode") == "recovery" or not status.get("os_installed", True)
        release_url = status.get("release_url", "")
        manual_mqtt_host = manual_option.get("mqtt_host", "192.168.1.153")
        manual_mqtt_port = manual_option.get("mqtt_port", 1883)
        if selected_profile == "manual":
            manual_mqtt_host = mqtt_cfg.get("host", manual_mqtt_host)
            manual_mqtt_port = mqtt_cfg.get("port", manual_mqtt_port)
        recovery_notice = ""
        if recovery_mode:
            recovery_notice = (
                '<p class="msg error">This device is in recovery mode. '
                'New Horizons OS must be written.</p>'
            )
        manual_fields_display = "block" if selected_profile == "manual" else "none"
        developer_checked = " checked" if selected_profile == "manual" else ""
        return """<!doctype html>
<html>
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>{title}</title>
  <style>{style}</style>
</head>
<body>
  <main>
      <p class="muted">{eyebrow}</p>
      <h1>{headline}</h1>
      <p class="msg">AP: <b>{ap_ssid}</b><br>URL: <b>{portal_url}</b></p>
      {recovery_notice}
      {notice}
      <h2>Wi-Fi</h2>
      <form method="post" action="/connect" onsubmit="showApplyOverlay();">
        <label for="ssid_select">Networks</label>
        <select id="ssid_select" onchange="document.getElementById('ssid').value = this.value;">
          {options}
        </select>
        <label class="check"><input id="developer_options" type="checkbox"{developer_checked} onchange="toggleDeveloperOptions(this.checked);"> Developer options</label>
        <label for="server_profile">Server</label>
        <select id="server_profile" name="server_profile" onchange="toggleManualServerFields(this.value);">
          {server_options}
        </select>
        <div id="manual_server_fields" style="display:{manual_fields_display}">
          <label for="mqtt_host">MQTT Host</label>
          <input id="mqtt_host" name="mqtt_host" value="{manual_mqtt_host}" placeholder="host">
          <label for="mqtt_port">MQTT Port</label>
          <input id="mqtt_port" name="mqtt_port" value="{manual_mqtt_port}" inputmode="numeric" placeholder="port">
        </div>
        <label for="ssid">Wi-Fi SSID</label>
        <input id="ssid" name="ssid" value="{ssid}" placeholder="ssid">
        <label for="password">Password</label>
        <input id="password" name="password" type="password" placeholder="password">
        <button type="submit">{primary_button}</button>
      </form>
      <div class="meta">
        <p class="muted">MQTT: {mqtt_host}:{mqtt_port}</p>
        <p class="muted">Release: {release_url}</p>
        <p class="muted">State: {device_state}</p>
      </div>
  </main>
  <div id="apply_overlay" class="overlay" role="status" aria-live="polite">
    <div>
      <div class="spinner"></div>
      <div>Applying settings<br>Do not touch the device power switch.</div>
    </div>
  </div>
  <script>
    function showApplyOverlay() {{
      var overlay = document.getElementById("apply_overlay");
      if (overlay) {{
        overlay.className = "overlay on";
      }}
    }}
    function toggleManualServerFields(value) {{
      var section = document.getElementById("manual_server_fields");
      if (!section) {{
        return;
      }}
      section.style.display = value === "manual" ? "block" : "none";
    }}
    function toggleDeveloperOptions(enabled) {{
      var select = document.getElementById("server_profile");
      if (!select) {{
        return;
      }}
      for (var i = 0; i < select.options.length; i++) {{
        var option = select.options[i];
        if (option.getAttribute("data-developer") === "1") {{
          option.disabled = !enabled;
        }}
      }}
      if (!enabled && select.value === "manual") {{
        select.value = "production";
      }}
      toggleManualServerFields(select.value);
    }}
    toggleDeveloperOptions({developer_enabled});
    toggleManualServerFields("{selected_profile}");
  </script>
</body>
</html>
""".format(
            title=_escape_html(self.config.SETUP_PORTAL_TITLE),
            style=INDEX_CSS,
            eyebrow="Recovery Mode" if recovery_mode else "Device Setup",
            headline="Write New Horizons OS" if recovery_mode else _escape_html(self.config.SETUP_PORTAL_TITLE),
            lead="This device is in recovery mode. Connect Wi-Fi, then write OS from GitHub through WebUI or MQTT." if recovery_mode else "Join the device hotspot, then use this page to connect the board to Wi-Fi and choose the MQTT broker. Most phones should auto-open this portal after joining the hotspot.",
            ip=ip_addr,
            notice=notice,
            recovery_notice=recovery_notice,
            options="".join(options),
            server_options="".join(server_options),
            ssid=_escape_html(selected_ssid),
            selected_profile=_escape_html(selected_profile),
            ap_ssid=ap_ssid,
            saved_ssid=_escape_html(selected_ssid or "(none)"),
            device_state=_escape_html(status.get("state", "idle")),
            portal_url=portal_url,
            portal_domain=portal_domain or "(disabled)",
            manual_hint="" if not portal_domain else " Fallback: <strong>{}</strong>.".format(portal_ip_url),
            manual_mqtt_host=_escape_html(manual_mqtt_host),
            manual_mqtt_port=_escape_html(manual_mqtt_port),
            mqtt_host=_escape_html(mqtt_cfg.get("host", "")),
            mqtt_port=_escape_html(mqtt_cfg.get("port", "")),
            release_url=_escape_html(release_url),
            primary_button="Save Recovery Settings" if recovery_mode else "Connect Wi-Fi",
            mqtt_tls_label="TLS" if mqtt_cfg.get("tls", False) else "plain",
            manual_fields_display=manual_fields_display,
            developer_checked=developer_checked,
            developer_enabled="true" if selected_profile == "manual" else "false",
        )

    def _render_result_page(self, result):
        ok = bool(result.get("ok"))
        title = "Wi-Fi connected" if ok else "Wi-Fi connection failed"
        cls = "ok" if ok else "error"
        message = result.get("message", "")
        ifconfig = result.get("ifconfig", ())
        details = ""
        if ifconfig:
            details = "<p class='muted'>IP: {}</p>".format(_escape_html(ifconfig[0]))
        return """<!doctype html>
<html>
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>{title}</title>
  <style>{style}</style>
</head>
<body>
  <main>
      <h1 class="{cls}">{title}</h1>
      <p>{message}</p>
      {details}
      <p><a href="/">Back to setup page</a></p>
  </main>
</body>
</html>
""".format(
            style=RESULT_CSS,
            cls=cls,
            title=_escape_html(title),
            message=_escape_html(message),
            details=details,
        )
