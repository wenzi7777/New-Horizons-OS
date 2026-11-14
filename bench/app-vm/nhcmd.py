"""Send one read-only control command to a device over UDP and print the reply."""
import json, socket, sys, time, itertools
_seq = itertools.count(int(time.time()) % 100000)

def cmd(ip, command, timeout=5.0, **args):
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM); s.settimeout(0.3)
    n = next(_seq); rid = f"bench-{n}"
    payload = {"command": command, "quiet": True, **args}
    s.sendto(json.dumps({"type": "command", "seq": n, "request_id": rid, "payload": payload}).encode(), (ip, 13250))
    chunks = {}; total = None; end = time.time() + timeout
    while time.time() < end:
        try: d, _ = s.recvfrom(4096)
        except socket.timeout: continue
        try: m = json.loads(d)
        except Exception: continue
        if m.get("request_id") != rid: continue
        if m.get("type") == "result": return m["payload"]
        if m.get("type") == "result_chunk":
            chunks[m["chunk"]] = m["data"]; total = m["chunks"]
            if len(chunks) == total:
                return json.loads("".join(chunks[i] for i in range(total)))
    return None

if __name__ == "__main__":
    print(json.dumps(cmd(sys.argv[1], sys.argv[2]), ensure_ascii=False)[:3000])
