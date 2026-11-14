"""Install App Library packages on a device over UDP, replacing what is there,
and activate them in slots 0..n-1. Enters and leaves maintenance mode.
usage: install_apps.py <ip> <library-root> <id> [<id> ...]"""
import hashlib, json, sys, time
sys.path.insert(0, "."); import nhcmd

ip, root, ids = sys.argv[1], sys.argv[2], sys.argv[3:]
CHUNK = 96  # the device reads at most 511 bytes of a UDP command


def must(reply, what):
    if not reply or not reply.get("ok"):
        raise SystemExit(f"{what}: {json.dumps(reply)[:400]}")
    return reply


print("enter_maintenance", json.dumps(nhcmd.cmd(ip, "enter_maintenance", timeout=6))[:120])
try:
    for slot, app in enumerate(ids):
        data = open(f"{root}/apps/{app}/app.nha", "rb").read()
        sha = hashlib.sha256(data).hexdigest()
        path = f"apps/{app}.nha"
        # Whatever holds the slot, and the old package itself, step aside first.
        nhcmd.cmd(ip, "app_deactivate", timeout=6, id=app, slot=-1)
        must(nhcmd.cmd(ip, "file_write_begin", timeout=6, scope="user", path=path, size=len(data)), "file_write_begin")
        for off in range(0, len(data), CHUNK):
            must(nhcmd.cmd(ip, "file_write_chunk", timeout=6, scope="user", path=path, offset=off,
                           data=data[off:off + CHUNK].hex()), f"chunk {off}")
        must(nhcmd.cmd(ip, "file_write_finish", timeout=8, scope="user", path=path, sha256=sha), "file_write_finish")
        r = must(nhcmd.cmd(ip, "app_install", timeout=10, path=path, sha256=sha, replace=True), f"install {app}")
        print("installed", app, r.get("message"))
        r = nhcmd.cmd(ip, "app_activate", timeout=10, id=app, slot=slot)
        print("activate", app, slot, json.dumps(r)[:300])
finally:
    print("exit_maintenance", json.dumps(nhcmd.cmd(ip, "exit_maintenance", timeout=6))[:200])

r = nhcmd.cmd(ip, "app_list_packages", timeout=6)
for p in r["data"].get("packages", []):
    print(p.get("id"), p.get("version"), p.get("state"), p.get("slot"), p.get("estimated_us"))
