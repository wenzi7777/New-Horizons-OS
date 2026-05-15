# storage.py
import json
import os

try:
    import hashlib
except ImportError:  # pragma: no cover - MicroPython fallback
    hashlib = None

try:
    import ubinascii as binascii
except ImportError:
    import binascii


def _norm(path):
    if not path:
        return "."
    return path.replace("\\", "/")


def dirname(path):
    path = _norm(path)
    if "/" not in path:
        return ""
    return path.rsplit("/", 1)[0]


def ensure_dir(path):
    path = _norm(path)
    if not path or path == ".":
        return

    parts = [part for part in path.split("/") if part]
    current = "/" if path.startswith("/") else ""

    for part in parts:
        if current in ("", "/"):
            current = current + part if current == "/" else part
        else:
            current = current + "/" + part
        try:
            os.mkdir(current)
        except OSError:
            pass


def exists(path):
    try:
        os.stat(path)
        return True
    except OSError:
        return False


def load_json(path, default=None):
    try:
        with open(path, "r") as f:
            return json.load(f)
    except (OSError, ValueError):
        return default


def save_json(path, data):
    ensure_dir(dirname(path))
    tmp = path + ".tmp"

    with open(tmp, "w") as f:
        json.dump(data, f)

    try:
        os.remove(path)
    except OSError:
        pass

    os.rename(tmp, path)


def read_text(path, default=None):
    try:
        with open(path, "r") as f:
            return f.read()
    except OSError:
        return default


def read_bytes(path, default=None):
    try:
        with open(path, "rb") as f:
            return f.read()
    except OSError:
        return default


def write_text(path, text):
    ensure_dir(dirname(path))
    tmp = path + ".tmp"
    with open(tmp, "w") as f:
        f.write(text)
    try:
        os.remove(path)
    except OSError:
        pass
    os.rename(tmp, path)


def write_bytes(path, data):
    ensure_dir(dirname(path))
    tmp = path + ".tmp"
    with open(tmp, "wb") as f:
        f.write(data)
    try:
        os.remove(path)
    except OSError:
        pass
    os.rename(tmp, path)


def remove(path):
    try:
        os.remove(path)
        return True
    except OSError:
        return False


def list_tree(root):
    root = _norm(root)
    items = []

    if not exists(root):
        return items

    def walk(base):
        try:
            entries = os.listdir(base)
        except OSError:
            return

        for name in entries:
            full = base.rstrip("/") + "/" + name if base not in ("", "/") else base + name
            rel = full[len(root):].lstrip("/") if full.startswith(root) else full
            try:
                mode = os.stat(full)[0]
            except OSError:
                continue
            is_dir = bool(mode & 0x4000)
            items.append({
                "path": rel,
                "is_dir": is_dir,
                "size": 0 if is_dir else os.stat(full)[6],
            })
            if is_dir:
                walk(full)

    walk(root)
    return items


def sha256_hex_bytes(data):
    if hashlib is None:
        raise RuntimeError("hashlib unavailable")
    digest = hashlib.sha256(data)
    if hasattr(digest, "hexdigest"):
        return digest.hexdigest()
    return binascii.hexlify(digest.digest()).decode()


def sha256_hex_file(path):
    data = read_bytes(path)
    if data is None:
        return None
    return sha256_hex_bytes(data)
