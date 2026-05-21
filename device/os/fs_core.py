# fs_core.py
try:
    import uos as os
except ImportError:  # pragma: no cover - CPython fallback
    import os


def _norm(path):
    if not path:
        return "."
    return str(path).replace("\\", "/")


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


def file_size(path):
    try:
        return int(os.stat(path)[6])
    except OSError:
        return None


def remove(path):
    try:
        os.remove(path)
        return True
    except OSError:
        return False


def rename(src, dst):
    ensure_dir(dirname(dst))
    try:
        remove(dst)
    except Exception:
        pass
    os.rename(src, dst)


def list_names(path):
    try:
        return list(os.listdir(path))
    except OSError:
        return []


def statvfs_usage(path="/"):
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


def fs_usage(path="/"):
    return statvfs_usage(path)


def safe_join(root, relative_path, allow_empty=False):
    root = _norm(root).rstrip("/")
    raw = _norm(relative_path)
    if raw.startswith("/"):
        raise ValueError("invalid_path")
    rel = raw.strip("/")
    parts = [part for part in rel.split("/") if part]
    if not parts:
        if allow_empty:
            return root
        raise ValueError("invalid_path")
    if any(part in (".", "..") for part in parts):
        raise ValueError("invalid_path")
    return root + "/" + "/".join(parts) if root else "/".join(parts)
