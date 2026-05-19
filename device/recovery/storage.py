import json
try:
    import uos as os
except ImportError:  # pragma: no cover - CPython fallback
    import os

try:
    import hashlib
except ImportError:
    hashlib = None

try:
    import ubinascii as binascii
except ImportError:
    import binascii


def _norm(path):
    return "." if not path else path.replace("\\", "/")


def dirname(path):
    path = _norm(path)
    return "" if "/" not in path else path.rsplit("/", 1)[0]


def ensure_dir(path):
    path = _norm(path)
    if not path or path == ".":
        return
    parts = [part for part in path.split("/") if part]
    current = "/" if path.startswith("/") else ""
    for part in parts:
        current = current + part if current in ("", "/") else current + "/" + part
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


def file_size(path):
    try:
        return int(os.stat(path)[6])
    except OSError:
        return None


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
    try:
        os.remove(path)
        return True
    except OSError:
        return False


def list_tree(root):
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
                stat = os.stat(full)
            except OSError:
                continue
            is_dir = bool(stat[0] & 0x4000)
            items.append({"path": rel, "is_dir": is_dir, "size": 0 if is_dir else stat[6]})
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
    try:
        stats = os.statvfs(path)
        block_size = int(stats[0] or stats[1] or 1)
        total = int(stats[2]) * block_size
        free = int(stats[3]) * block_size
        used = max(0, total - free)
    except (AttributeError, OSError, TypeError, ValueError):
        total = 0
        free = 0
        used = 0
    return {
        "total_bytes": total,
        "used_bytes": used,
        "free_bytes": free,
        "percent_used": int((used * 100) // total) if total else 0,
    }


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
