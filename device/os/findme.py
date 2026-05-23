"""New Horizons FindMe LAN gateway selection."""

import socket
import time

import config
import nhcp


DISCOVER_TYPE = "findme_discover"
OFFER_TYPE = "findme_offer"
PROTO_VERSION = 1


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


def _send_frame(sock, payload, address):
    sock.sendto(nhcp.encode_frame(DISCOVER_TYPE, device_uid=payload.get("device_uid", ""), payload=payload), address)


def _decode_offer(data):
    try:
        frame = nhcp.decode_frame(data)
    except Exception:
        return None
    if frame.get("type") != OFFER_TYPE:
        return None
    payload = frame.get("payload", {})
    if not isinstance(payload, dict):
        return None
    return payload


def _offer_score(offer):
    return (
        1 if offer.get("claim_match") else 0,
        _as_int(offer.get("priority"), 0),
        -_as_int(offer.get("latency_ms"), 999999),
    )


def _upstream_healthy(offer):
    upstream = str(offer.get("upstream_status", "") or "").lower()
    return upstream not in ("offline", "down", "error", "unhealthy")


def discover(
    device_uid,
    mode,
    device_name="",
    versions=None,
    wifi_rssi=None,
    rejected_gateways=None,
    timeout_ms=None,
    attempts=None,
    discovery_port=None,
    preferred_gateway_id="",
    claim_id="",
    claim_expires_at_ms=0,
):
    """Broadcast FindMe discovery and return the best accepted gateway offer."""
    timeout_ms = timeout_ms or getattr(config, "GATEWAY_DISCOVERY_TIMEOUT_MS", 1500)
    attempts = attempts or getattr(config, "GATEWAY_DISCOVERY_ATTEMPTS", 2)
    discovery_port = discovery_port or getattr(config, "DEFAULT_GATEWAY_DISCOVERY_PORT", 22346)
    tcp_default = getattr(config, "DEFAULT_TCP_CONTROL_PORT", 22345)
    udp_default = getattr(config, "DEFAULT_UDP_STREAM_PORT", 13250)
    versions = versions or {}
    rejected_gateways = rejected_gateways or []
    preferred_gateway_id = str(preferred_gateway_id or "")
    claim_id = str(claim_id or "")
    claim_active = bool(preferred_gateway_id and claim_id)
    if claim_active and claim_expires_at_ms:
        try:
            claim_active = _ticks_diff(int(claim_expires_at_ms), _now_ms()) > 0
        except Exception:
            claim_active = True
    request = {
        "type": DISCOVER_TYPE,
        "version": PROTO_VERSION,
        "device_uid": device_uid,
        "device_name": device_name,
        "mode": mode,
        "runtime_version": versions.get("runtime_version", ""),
        "recovery_version": versions.get("recovery_version", ""),
        "os_version": versions.get("os_version", ""),
        "wifi_rssi": wifi_rssi,
    }
    if claim_active:
        request["preferred_gateway_id"] = preferred_gateway_id
        request["claim_id"] = claim_id
    sock = None
    accepted = []
    rejected = []
    last_error = ""
    rejected_lookup = {}
    for gateway_id in rejected_gateways:
        rejected_lookup[str(gateway_id)] = True

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
            start_ms = _now_ms()
            try:
                _send_frame(sock, request, ("255.255.255.255", discovery_port))
            except Exception as exc:
                last_error = "findme_broadcast_failed:%s" % exc
                continue
            deadline = start_ms + timeout_ms
            while _ticks_diff(deadline, _now_ms()) > 0:
                try:
                    data, addr = sock.recvfrom(1024)
                except Exception as exc:
                    last_error = "findme_timeout:%s" % exc
                    break
                latency_ms = max(0, _ticks_diff(_now_ms(), start_ms))
                obj = _decode_offer(data)
                if obj is None:
                    continue
                gateway_id = str(obj.get("gateway_id") or "")
                offer = {
                    "host": addr[0],
                    "tcp_port": _as_int(obj.get("tcp_port"), tcp_default),
                    "udp_port": _as_int(obj.get("udp_port"), udp_default),
                    "gateway_id": gateway_id,
                    "gateway_name": str(obj.get("gateway_name") or "New Horizons Gateway"),
                    "priority": _as_int(obj.get("priority"), 0),
                    "upstream_status": str(obj.get("upstream_status") or ""),
                    "ttl_ms": _as_int(obj.get("ttl_ms"), 10000),
                    "latency_ms": latency_ms,
                    "source": "findme",
                    "discovered_at_ms": _now_ms(),
                }
                offered_claim_id = str(obj.get("claim_id") or "")
                claim_match = bool(claim_active and offered_claim_id == claim_id and gateway_id == preferred_gateway_id)
                if offered_claim_id:
                    offer["claim_id"] = offered_claim_id
                if claim_match:
                    offer["claim_match"] = True
                if obj.get("accept") is False:
                    offer["reason"] = str(obj.get("reason") or "rejected")
                    offer["cooldown_ms"] = _as_int(obj.get("cooldown_ms"), 30000)
                    rejected.append(offer)
                    continue
                if claim_active and not claim_match:
                    offer["reason"] = "claim_not_matching"
                    rejected.append(offer)
                    continue
                if not _upstream_healthy(offer):
                    offer["reason"] = "upstream_unhealthy"
                    rejected.append(offer)
                    continue
                if gateway_id and rejected_lookup.get(gateway_id):
                    offer["reason"] = "cooldown"
                    rejected.append(offer)
                    continue
                accepted.append(offer)
            if accepted:
                break
    finally:
        if sock is not None:
            try:
                sock.close()
            except Exception:
                pass

    if not accepted:
        return {
            "ok": False,
            "error": "findme_claim_timeout" if claim_active else ("findme_rejected" if rejected else (last_error or "findme_no_gateway")),
            "rejected_gateways": rejected,
        }

    best = sorted(accepted, key=_offer_score, reverse=True)[0]
    best["ok"] = True
    best["rejected_gateways"] = rejected
    return best
