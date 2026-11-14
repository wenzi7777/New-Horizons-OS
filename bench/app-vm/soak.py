"""Long soak log of a device running the App VM: one CSV row a minute,
flushed every row, so it can be read while it runs.
usage: soak.py <ip> <hours> <out.csv>"""
import csv, json, re, sys, time
sys.path.insert(0, "."); import nhcmd
ip, hours, out = sys.argv[1], float(sys.argv[2]), sys.argv[3]
cols = ["time", "uptime_s", "boot_count", "reset_reason", "fw", "heap_free", "heap_largest", "heap_min_free",
        "heap_min_largest", "scan_fps", "produced", "dropped", "overrun", "apps_last_us", "apps_max_us",
        "apps", "faults", "suspensions", "frames_skipped", "app_overruns", "rtt_ms"]
f = open(out, "a", newline=""); w = csv.writer(f)
if f.tell() == 0: w.writerow(cols)
end = time.time() + hours * 3600
while time.time() < end:
    a = time.time(); m = nhcmd.cmd(ip, "memory_status", timeout=4); rtt = (time.time() - a) * 1000
    s = nhcmd.cmd(ip, "status", timeout=8)
    l = nhcmd.cmd(ip, "app_list", timeout=8, outputs="none")
    now = time.strftime("%Y-%m-%d %H:%M:%S")
    if not (m and s and l):
        w.writerow([now, "timeout"]); f.flush(); time.sleep(60); continue
    mem, st, ap = m["data"], s["data"], l["data"]
    t = json.dumps(st)
    g = lambda k: (re.findall(r'"%s": "?([^,"}]*)' % k, t) or [""])[0]
    sh = st.get("scan_health", {})
    apps = ap.get("apps", [])
    vm = [x.get("state_detail", {}).get("vm", {}) for x in apps]
    w.writerow([now, int(int(g("uptime_ms") or 0) / 1000), g("boot_count"), g("last_reset_reason"), st.get("firmware_version"),
                mem["heap_free"], mem["heap_largest_free_block"], mem["heap_min_free"], mem.get("heap_min_largest_free_block", ""),
                sh.get("actual_scan_fps"), sh.get("produced_frames"), sh.get("dropped_frames"), sh.get("overrun_frames"),
                ap.get("last_total_us"), ap.get("max_total_us"),
                " ".join(f"{x.get('name')}:{x.get('state')}" for x in apps),
                " ".join(v.get("fault", "") for v in vm), " ".join(str(v.get("suspensions", "")) for v in vm),
                " ".join(str(v.get("frames_skipped", "")) for v in vm), " ".join(str(x.get("overruns", "")) for x in apps),
                round(rtt, 1)])
    f.flush(); time.sleep(60)
