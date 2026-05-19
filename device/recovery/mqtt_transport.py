import json
import time

from umqtt.simple import MQTTClient


class MQTTTransport:
    MAX_PENDING = 8

    def __init__(self, runtime_getter, device_uid, logger=None):
        self.runtime_getter = runtime_getter
        self.device_uid = device_uid
        self.logger = logger
        self.client = None
        self.client_key = None
        self.pending = []
        self.last_attempt_ms = 0

    def enabled(self):
        runtime = self.runtime_getter()
        return runtime.get("transport", {}).get("mode", "udp") == "mqtt"

    def poll(self, wifi_connected, handler=None):
        if not self.ensure_connected(wifi_connected):
            return False
        try:
            self.client.check_msg()
        except Exception as exc:
            self._warn("mqtt_check_msg_failed {}".format(exc))
            self.close()
            return False
        if handler is not None:
            while self.pending:
                request = self.pending.pop(0)
                command = self._command_name(request)
                self._info("mqtt_command_received command={}".format(command))
                try:
                    response = handler(request, ("mqtt", 0))
                    self._info(
                        "mqtt_command_done command={} status={} message={}".format(
                            command,
                            self._response_value(response, "status"),
                            self._response_value(response, "message"),
                        )
                    )
                except Exception as exc:
                    self._warn("mqtt_command_failed command={} error={}".format(command, exc))
                    response = {
                        "status": "error",
                        "message": "command_failed",
                        "command": command,
                        "error": str(exc),
                        "reboot_required": False,
                    }
                if response is not None:
                    self.publish_result(response, wifi_connected)
        return True

    def publish_status(self, payload, wifi_connected):
        if not self.ensure_connected(wifi_connected):
            return False
        return self._publish_json(self._topic("status"), payload)

    def publish_result(self, payload, wifi_connected):
        if not self.ensure_connected(wifi_connected):
            return False
        return self._publish_json(self._topic("result"), payload)

    def close(self):
        if self.client is not None:
            try:
                self.client.disconnect()
            except Exception:
                pass
        self.client = None
        self.client_key = None

    def reconfigure(self):
        self.last_attempt_ms = 0
        self.close()

    def ensure_connected(self, wifi_connected):
        if not self.enabled() or not wifi_connected:
            self.close()
            return False
        runtime = self.runtime_getter()
        mqtt_cfg = runtime.get("mqtt", {})
        transport_cfg = runtime.get("transport", {})
        host = mqtt_cfg.get("host", "")
        port = int(mqtt_cfg.get("port", 8883))
        tls = bool(mqtt_cfg.get("tls", True))
        user = mqtt_cfg.get("username") or None
        password = mqtt_cfg.get("password") or None
        namespace = transport_cfg.get("topic_namespace", "newhorizons/v1")
        if not host:
            return False
        key = (host, port, tls, user, password, namespace, self.device_uid)
        if self.client is not None and self.client_key == key:
            return True
        now = time.ticks_ms()
        if time.ticks_diff(now, self.last_attempt_ms) < 5000:
            return False
        self.last_attempt_ms = now
        self.close()
        try:
            client = MQTTClient(
                client_id="nh-min-{}".format(self.device_uid[-8:]),
                server=host,
                port=port,
                user=user,
                password=password,
                keepalive=30,
                ssl=tls,
                ssl_params={},
            )
            client.set_callback(self._on_message)
            client.connect()
            client.subscribe(self._topic("cmd", namespace=namespace), qos=1)
            self.client = client
            self.client_key = key
            self._info("mqtt_connected host={} port={}".format(host, port))
            return True
        except Exception as exc:
            self._warn("mqtt_connect_failed {}".format(exc))
            self.close()
            return False

    def _topic(self, kind, namespace=None):
        runtime = self.runtime_getter()
        transport_cfg = runtime.get("transport", {})
        base = (namespace or transport_cfg.get("topic_namespace") or "newhorizons/v1").rstrip("/")
        return "{}/device/{}/{}".format(base, self.device_uid, kind)

    def _publish_json(self, topic, payload):
        try:
            self.client.publish(topic.encode(), json.dumps(payload).encode(), qos=1)
            return True
        except Exception as exc:
            self._warn("mqtt_publish_json_failed {}".format(exc))
            self.close()
            return False

    def _on_message(self, topic, payload):
        try:
            data = json.loads(payload.decode())
        except Exception as exc:
            self._warn("mqtt_decode_failed {}".format(exc))
            return
        if isinstance(data, dict):
            if len(self.pending) >= self.MAX_PENDING:
                self.pending.pop(0)
            self.pending.append(data)

    def _command_name(self, request):
        if not isinstance(request, dict):
            return "unknown"
        return str(request.get("command", request.get("cmd", "unknown")) or "unknown")

    def _response_value(self, response, key):
        if not isinstance(response, dict):
            return ""
        value = response.get(key, "")
        return "" if value is None else value

    def _info(self, message):
        if self.logger:
            self.logger.info(message)
        else:
            print(message)

    def _warn(self, message):
        if self.logger:
            self.logger.warn(message)
        else:
            print(message)
