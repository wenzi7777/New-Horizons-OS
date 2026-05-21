"""LAN gateway discovery for New Horizons devices."""

import json
import socket
import time

import config


DISCOVER_TYPE = "newhorizons_discover"
GATEWAY_TYPE = "newhorizons_gateway"


def _now_ms():
    try:
        return time.ticks_ms()
    except AttributeError:
        return int(time.time() * 1000)


def _ticks_diff(a, b):
    try:
        return time.ticks_diff(a, b)
    except AttributeError:
        return a - b


def _as_int(value, default):
    try:
        return int(value)
    except Exception:
        return default


def _send_json(sock, payload, address):
    body = json.dumps(payload).encode("utf-8")
    sock.sendto(body, address)


def discover_gateway(device_uid, mode, timeout_ms=None, attempts=None, discovery_port=None):
    """Broadcast a LAN discovery request and return the best gateway response."""
    timeout_ms = timeout_ms or getattr(config, "GATEWAY_DISCOVERY_TIMEOUT_MS", 1500)
    attempts = attempts or getattr(config, "GATEWAY_DISCOVERY_ATTEMPTS", 2)
    discovery_port = discovery_port or getattr(config, "DEFAULT_GATEWAY_DISCOVERY_PORT", 22346)
    tcp_default = getattr(config, "DEFAULT_TCP_CONTROL_PORT", 22345)
    udp_default = getattr(config, "DEFAULT_UDP_STREAM_PORT", 13250)
    request = {
        "type": DISCOVER_TYPE,
        "version": 1,
        "device_uid": device_uid,
        "mode": mode,
    }
    sock = None
    best = None
    best_priority = -1000000
    last_error = ""
    try:
        sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        try:
            sock.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
        except Exception:
            pass
        try:
            sock.settimeout(max(timeout_ms / 1000.0, 0.05))
        except Exception:
            pass

        for _ in range(max(1, attempts)):
            try:
                _send_json(sock, request, ("255.255.255.255", discovery_port))
            except Exception as exc:
                last_error = "broadcast_failed:%s" % exc
                continue
            deadline = _now_ms() + timeout_ms
            while _ticks_diff(deadline, _now_ms()) > 0:
                try:
                    data, addr = sock.recvfrom(768)
                except Exception as exc:
                    last_error = "timeout:%s" % exc
                    break
                try:
                    obj = json.loads(data.decode("utf-8"))
                except Exception:
                    continue
                if not isinstance(obj, dict) or obj.get("type") != GATEWAY_TYPE:
                    continue
                if _as_int(obj.get("version"), 0) != 1:
                    continue
                priority = _as_int(obj.get("priority"), 0)
                if best is not None and priority <= best_priority:
                    continue
                best_priority = priority
                best = {
                    "host": addr[0],
                    "tcp_port": _as_int(obj.get("tcp_port"), tcp_default),
                    "udp_port": _as_int(obj.get("udp_port"), udp_default),
                    "gateway_id": str(obj.get("gateway_id") or ""),
                    "priority": priority,
                    "source": "discovery",
                    "discovered_at_ms": _now_ms(),
                }
            if best is not None:
                break
    finally:
        if sock is not None:
            try:
                sock.close()
            except Exception:
                pass

    if best is None:
        return {"ok": False, "error": last_error or "no_gateway"}
    best["ok"] = True
    return best
