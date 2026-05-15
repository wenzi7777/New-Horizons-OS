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


CAPTIVE_PORTAL_PATHS = (
    "/generate_204",
    "/hotspot-detect.html",
    "/library/test/success.html",
    "/connecttest.txt",
    "/ncsi.txt",
    "/success.txt",
    "/canonical.html",
)


class WiFiSetupPortal:
    def __init__(self, manager, config_module, logger=None):
        self.manager = manager
        self.config = config_module
        self.logger = logger
        self.server = None
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
        self.active = True
        return True

    def stop(self):
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

        try:
            client, _addr = self.server.accept()
        except OSError:
            return False

        try:
            client.settimeout(1)
        except Exception:
            pass

        try:
            method, path, body = self._read_request(client)
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
                )
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
            return False
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

        notice = ""
        if status.get("last_error"):
            notice = '<div class="notice error">Last error: {}</div>'.format(_escape_html(status["last_error"]))
        elif status.get("last_setup_result"):
            notice = '<div class="notice ok">Last result: {}</div>'.format(_escape_html(status["last_setup_result"]))

        ip_addr = _escape_html(status.get("portal_ip", self.config.SETUP_PORTAL_HOST))
        portal_url = _escape_html(status.get("portal_url", "http://{}".format(self.config.SETUP_PORTAL_HOST)))
        ap_ssid = _escape_html(status.get("ap_ssid", self.config.SETUP_AP_SSID_PREFIX))
        return """<!doctype html>
<html>
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>{title}</title>
  <style>
    * {{ box-sizing: border-box; }}
    body {{ font-family: -apple-system, BlinkMacSystemFont, sans-serif; margin: 0; background:
      radial-gradient(circle at top left, #233b67 0, #1a2740 28%, #0f1728 100%); color: #e7edf7; min-height: 100vh; }}
    .wrap {{ max-width: 640px; margin: 0 auto; padding: 24px 16px 40px; }}
    .hero {{ padding: 6px 2px 18px; }}
    .eyebrow {{ display: inline-block; padding: 6px 10px; border-radius: 999px; background: rgba(255,255,255,0.08); color: #a9c5ff; font-size: 12px; letter-spacing: 0.08em; text-transform: uppercase; }}
    h1 {{ margin: 14px 0 8px; font-size: 32px; line-height: 1.05; }}
    .lead {{ margin: 0; color: #c7d4ea; line-height: 1.55; }}
    .card {{ background: rgba(255,255,255,0.96); color: #142033; border-radius: 22px; padding: 22px; box-shadow: 0 18px 60px rgba(0,0,0,0.26); }}
    .grid {{ display: grid; gap: 12px; grid-template-columns: repeat(2, minmax(0, 1fr)); margin-bottom: 18px; }}
    .stat {{ background: #f4f7fb; border: 1px solid #dde6f3; border-radius: 14px; padding: 12px; }}
    .stat-label {{ font-size: 12px; color: #6b7788; text-transform: uppercase; letter-spacing: 0.06em; }}
    .stat-value {{ margin-top: 6px; font-size: 15px; font-weight: 700; word-break: break-word; }}
    .notice {{ margin: 0 0 16px; border-radius: 14px; padding: 12px 14px; font-weight: 600; }}
    .ok {{ background: #ecfdf3; color: #027a48; border: 1px solid #abefc6; }}
    .error {{ background: #fef3f2; color: #b42318; border: 1px solid #fecdca; }}
    .section-title {{ margin: 4px 0 8px; font-size: 14px; color: #506074; text-transform: uppercase; letter-spacing: 0.08em; }}
    label {{ display: block; margin: 14px 0 6px; font-weight: 700; }}
    input, select, button {{ width: 100%; border-radius: 14px; border: 1px solid #c8d2e1; padding: 13px 14px; font-size: 16px; }}
    input, select {{ background: #fff; }}
    button {{ background: linear-gradient(135deg, #163157, #254f8c); color: #fff; border: 0; margin-top: 18px; font-weight: 800; box-shadow: 0 10px 24px rgba(22,49,87,0.25); }}
    .muted {{ color: #5b6778; font-size: 14px; }}
    .hint {{ margin-top: 12px; padding: 12px 14px; border-radius: 14px; background: #eef4ff; color: #264372; font-size: 14px; line-height: 1.5; }}
    .meta {{ margin-top: 18px; padding-top: 16px; border-top: 1px solid #e7ecf3; }}
    .meta p {{ margin: 6px 0; }}
    @media (max-width: 560px) {{
      .grid {{ grid-template-columns: 1fr; }}
      h1 {{ font-size: 28px; }}
      .card {{ padding: 18px; border-radius: 18px; }}
    }}
  </style>
</head>
<body>
  <div class="wrap">
    <div class="hero">
      <div class="eyebrow">Device Setup</div>
      <h1>{title}</h1>
      <p class="lead">Join the device hotspot, then use this page to connect the board to your local Wi-Fi. Most phones should auto-open this portal after joining the hotspot.</p>
    </div>
    <div class="card">
      <div class="grid">
        <div class="stat">
          <div class="stat-label">Hotspot SSID</div>
          <div class="stat-value">{ap_ssid}</div>
        </div>
        <div class="stat">
          <div class="stat-label">Portal URL</div>
          <div class="stat-value">{portal_url}</div>
        </div>
      </div>
      {notice}
      <div class="section-title">Wi-Fi Connection</div>
      <form method="post" action="/connect">
        <label for="ssid_select">Detected networks</label>
        <select id="ssid_select" onchange="document.getElementById('ssid').value = this.value;">
          {options}
        </select>
        <label for="ssid">Wi-Fi SSID</label>
        <input id="ssid" name="ssid" value="{ssid}" placeholder="Your Wi-Fi name">
        <label for="password">Wi-Fi password</label>
        <input id="password" name="password" type="password" placeholder="Your Wi-Fi password">
        <button type="submit">Connect Wi-Fi</button>
      </form>
      <div class="hint">
        If this page did not open automatically, use <strong>{portal_url}</strong> in your browser while connected to the hotspot.
      </div>
      <div class="meta">
        <p class="muted">Portal IP: {ip}</p>
        <p class="muted">Saved SSID: {saved_ssid}</p>
        <p class="muted">Device state: {device_state}</p>
      </div>
    </div>
  </div>
</body>
</html>
""".format(
            title=_escape_html(self.config.SETUP_PORTAL_TITLE),
            ip=ip_addr,
            notice=notice,
            options="".join(options),
            ssid=_escape_html(selected_ssid),
            ap_ssid=ap_ssid,
            saved_ssid=_escape_html(selected_ssid or "(none)"),
            device_state=_escape_html(status.get("state", "idle")),
            portal_url=portal_url,
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
  <style>
    body {{ font-family: -apple-system, BlinkMacSystemFont, sans-serif; margin: 0; background: #f6f8fb; color: #142033; }}
    .wrap {{ max-width: 560px; margin: 0 auto; padding: 24px 18px 48px; }}
    .card {{ background: #fff; border-radius: 16px; padding: 20px; box-shadow: 0 10px 35px rgba(20,32,51,0.08); }}
    .ok {{ color: #0b7a3c; }}
    .error {{ color: #b42318; }}
    a {{ color: #13294b; font-weight: 700; }}
  </style>
</head>
<body>
  <div class="wrap">
    <div class="card">
      <h1 class="{cls}">{title}</h1>
      <p>{message}</p>
      {details}
      <p><a href="/">Back to setup page</a></p>
    </div>
  </div>
</body>
</html>
""".format(
            cls=cls,
            title=_escape_html(title),
            message=_escape_html(message),
            details=details,
        )
