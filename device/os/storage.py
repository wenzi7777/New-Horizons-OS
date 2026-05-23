# storage.py
try:
    import uos as os
except ImportError:  # pragma: no cover - CPython fallback
    import os

import fs_core
import nhcp

try:
    import hashlib
except ImportError:  # pragma: no cover - MicroPython fallback
    hashlib = None

try:
    import ubinascii as binascii
except ImportError:
    import binascii

TLV_MAGIC = nhcp.TLV_MAGIC


def _norm(path):
    return fs_core._norm(path)


def dirname(path):
    return fs_core.dirname(path)


def ensure_dir(path):
    return fs_core.ensure_dir(path)


def exists(path):
    return fs_core.exists(path)


def loads_tlv(data):
    return nhcp.decode_tlv(data)


def dumps_tlv(data):
    return nhcp.encode_tlv(data)


def load_tlv(path, default=None):
    try:
        with open(path, "rb") as f:
            return loads_tlv(f.read())
    except Exception:
        return default


def save_tlv(path, data):
    ensure_dir(dirname(path))
    tmp = path + ".tmp"

    with open(tmp, "wb") as f:
        f.write(dumps_tlv(data))

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


def file_size(path):
    return fs_core.file_size(path)


def read_chunk(path, offset=0, length=1024):
    with open(path, "rb") as f:
        if offset:
            f.seek(int(offset))
        return f.read(int(length))


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
    return fs_core.remove(path)


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


def tree_size(root):
    total = 0
    for item in list_tree(root):
        if not item.get("is_dir"):
            total += int(item.get("size", 0) or 0)
    return total


def fs_usage(path="/"):
    return fs_core.fs_usage(path)


def sha256_hex_bytes(data):
    if hashlib is None:
        raise RuntimeError("hashlib unavailable")
    digest = hashlib.sha256(data)
    if hasattr(digest, "hexdigest"):
        return digest.hexdigest()
    return binascii.hexlify(digest.digest()).decode()


def sha256_hex_file(path):
    if hashlib is None:
        raise RuntimeError("hashlib unavailable")
    try:
        digest = hashlib.sha256()
        with open(path, "rb") as f:
            while True:
                chunk = f.read(1024)
                if not chunk:
                    break
                digest.update(chunk)
    except OSError:
        return None
    if hasattr(digest, "hexdigest"):
        return digest.hexdigest()
    return binascii.hexlify(digest.digest()).decode()
