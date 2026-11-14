"""Poll a device's sysmon + memory_status for a baseline; write CSV.
usage: poll.py <ip> <seconds> <out.csv>"""
import csv, sys, time
sys.path.insert(0, "."); import nhcmd
ip, secs, out = sys.argv[1], float(sys.argv[2]), sys.argv[3]
cols = ["t_s","rtt_ms","heap_free","heap_largest","heap_min_free","heap_min_largest","scan_fps","produced","dropped","overrun",
        "scan_last_us","scan_max_us","apps_last_us","apps_max_us","apps_budget_us","max_tick_us","loop_busy","scan_task_max_us","apps_task_max_us","running_apps"]
w = csv.writer(open(out, "w", newline="")); w.writerow(cols)
t0 = time.time()
while time.time() - t0 < secs:
    a = time.time(); m = nhcmd.cmd(ip, "memory_status", timeout=3); rtt = (time.time() - a) * 1000
    r = nhcmd.cmd(ip, "sysmon", timeout=8)
    if not (m and r and r.get("ok")):
        w.writerow([round(time.time()-t0,1), "timeout"]); time.sleep(5); continue
    d = r["data"]; mem = m["data"]; sc = d.get("scan", {}); ap = d.get("apps", {}); sch = d.get("scheduler", {})
    tasks = {t["name"]: t for t in sch.get("tasks", [])}
    w.writerow([round(time.time()-t0,1), round(rtt,1), mem["heap_free"], mem["heap_largest_free_block"], mem["heap_min_free"],
                mem.get("heap_min_largest_free_block",""), sc.get("actual_scan_fps"), sc.get("produced_frames"), sc.get("dropped_frames"),
                sc.get("overrun_frames"), sc.get("last_scan_duration_us"), sc.get("max_scan_duration_us"), ap.get("last_total_us"),
                ap.get("max_total_us"), ap.get("budget_us"), sch.get("max_tick_us"), d.get("loop_busy_permille"),
                tasks.get("scan_stream",{}).get("max_us"), tasks.get("apps",{}).get("max_us"),
                sum(1 for x in ap.get("apps",[]) if x.get("state")=="running" and not x.get("idle"))])
    sys.stdout.flush(); time.sleep(5)
