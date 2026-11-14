"""Read the PROFILE counters of a profiling build: reset, wait, read.
usage: prof.py <ip> [seconds]"""
import json, re, sys, time
sys.path.insert(0, "."); import nhcmd
ip = sys.argv[1]; secs = float(sys.argv[2]) if len(sys.argv) > 2 else 30
src = open("../../firmware/newhorizons_os/AppContract.h").read()
body = src[src.index("enum class FlowOp"):]
body = body[:body.index("};")]
names = {}
for m in re.finditer(r"(\w+)\s*=\s*(\d+)", body): names[int(m.group(2))] = m.group(1)
nhcmd.cmd(ip, "app_list", timeout=8, outputs="flow")
time.sleep(secs)
r = nhcmd.cmd(ip, "app_list", timeout=8, outputs="flow")
d = r["data"]; print("total last/max us", d["last_total_us"], d["max_total_us"])
for a in d["apps"]:
    p = a.get("state_detail", {}).get("vm", {}).get("prof")
    if not p: print(a["name"], "no prof"); continue
    us = lambda c: round(c / 240, 1)
    print(f"{a['name']}: last {a.get('last_us')} max {a.get('max_us')} | n={p['n']} deliver {us(p['deliver'])}us run {us(p['run'])}us nat {us(p['nat'])}us "
          f"interp {us(p['run']-p['nat'])}us outside-run {us(p['deliver']-p['run'])}us insns {p['insns']} nats {p['nats']}")
    for op, (calls, cyc) in sorted(p["ops"].items(), key=lambda kv: -kv[1][0] * kv[1][1]):
        print(f"    {names.get(int(op), op):>18} calls {calls:6d}  {us(cyc):7.1f} us/call")
