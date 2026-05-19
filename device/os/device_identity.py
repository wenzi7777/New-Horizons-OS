try:
    import machine
except ImportError:
    machine = None

try:
    import network
except ImportError:
    network = None


_cached_mac_bytes = None


def _coerce_bytes(value):
    if value is None:
        return b""
    if isinstance(value, bytes):
        return value
    if isinstance(value, bytearray):
        return bytes(value)
    return bytes(value)


def mac_bytes(value=None):
    global _cached_mac_bytes
    if value is not None:
        return _coerce_bytes(value)
    if _cached_mac_bytes is not None:
        return _cached_mac_bytes
    if machine is not None:
        try:
            data = _coerce_bytes(machine.unique_id())
            if data:
                _cached_mac_bytes = data
                return data
        except Exception:
            pass
    if network is not None:
        for iface in (getattr(network, "STA_IF", None), getattr(network, "AP_IF", None)):
            if iface is None:
                continue
            try:
                wlan = network.WLAN(iface)
                mac = wlan.config("mac")
                if mac:
                    data = _coerce_bytes(mac)
                    _cached_mac_bytes = data
                    return data
            except Exception:
                pass
    return b""


def mac_hex(value=None):
    return "".join("{:02X}".format(b) for b in mac_bytes(value))


def derive_device_id(value=None):
    data = mac_bytes(value)
    if not data:
        return 0xA55A0001

    if len(data) >= 4:
        value = int.from_bytes(data[-4:], "big") & 0xFFFFFFFF
    else:
        value = 0
        for byte in data:
            value = ((value << 8) | byte) & 0xFFFFFFFF
    if value in (0, 0x00000001):
        value ^= 0xA55A5A5A
        value &= 0xFFFFFFFF
    return value


def get_device_id():
    return derive_device_id()


def get_device_uid():
    return mac_hex()


def get_device_suffix():
    uid = get_device_uid()
    if not uid:
        return ""
    return uid[-6:]


def get_device_name(base_name):
    suffix = get_device_suffix()
    if not suffix:
        return base_name
    return "{}-{}".format(base_name, suffix)
