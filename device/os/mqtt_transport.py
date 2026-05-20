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
        self.topic_cache = {}
        self.pending = []
        self.last_attempt_ms = 0

    def enabled(self):
        return True

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
                    response = self._annotate_response(request, response, command)
                    self.publish_result(response, wifi_connected)
        return True

    def publish_raw(self, payload, wifi_connected):
        if not wifi_connected or self.client is None:
            return False
        return self._publish_bytes(self._topic_bytes("raw"), payload, qos=0)

    def publish_status(self, payload, wifi_connected):
        if not self.ensure_connected(wifi_connected):
            return False
        return self._publish_json(self._topic_bytes("status"), payload, wifi_connected, qos=1)

    def publish_result(self, payload, wifi_connected):
        if not self.ensure_connected(wifi_connected):
            return False
        return self._publish_json(self._topic_bytes("result"), payload, wifi_connected, qos=1)

    def is_connected(self):
        return self.client is not None

    def close(self):
        if self.client is not None:
            try:
                self.client.disconnect()
            except Exception:
                pass
        self.client = None
        self.client_key = None
        self.topic_cache = {}

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
                client_id="nh-{}".format(self.device_uid[-10:]),
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
            self.topic_cache = {}
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
        if kind == "raw":
            return "{}/raw/{}".format(base, self.device_uid)
        return "{}/device/{}/{}".format(base, self.device_uid, kind)

    def _topic_bytes(self, kind):
        cached = self.topic_cache.get(kind)
        if cached is None:
            cached = self._topic(kind).encode()
            self.topic_cache[kind] = cached
        return cached

    def _publish_json(self, topic, payload, wifi_connected=True, qos=1):
        if not wifi_connected:
            return False
        try:
            topic_bytes = topic if isinstance(topic, bytes) else topic.encode()
            self.client.publish(topic_bytes, json.dumps(payload).encode(), qos=qos)
            return True
        except Exception as exc:
            self._warn("mqtt_publish_json_failed {}".format(exc))
            self.close()
            self.last_attempt_ms = 0
            return False

    def _publish_bytes(self, topic, payload, qos=0):
        try:
            topic_bytes = topic if isinstance(topic, bytes) else topic.encode()
            self.client.publish(topic_bytes, payload, qos=qos)
            return True
        except Exception as exc:
            self._warn("mqtt_publish_raw_failed {}".format(exc))
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

    def _annotate_response(self, request, response, command):
        if not isinstance(response, dict):
            return response
        if "command" not in response:
            response["command"] = command
        if isinstance(request, dict):
            request_id = request.get("request_id", "")
            if request_id and "request_id" not in response:
                response["request_id"] = request_id
        return response

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
