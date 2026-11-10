#!/usr/bin/env python3
"""
One producer sends the same 100-byte tasks to Redis (RPUSH onto a list) and
to Quicx (QXP SUBMIT) at 40,000 a second each, until both refuse or one is
killed. Then both are drained (Redis with LPOP, Quicx with 8 workers) and,
10 s later, the memory each container still uses is recorded.

usage: flood.py REDIS_CONTAINER QUICX_CONTAINER OUT.json   (see run.sh)
"""
import json, socket, struct, subprocess, sys, threading, time

R, Q, OUT = sys.argv[1:4]
RATE, BATCH = 40000, 400
PAYLOAD = b"x" * 100
ev = {"rate": RATE, "payload_bytes": len(PAYLOAD)}
now = time.time

def sh(*a): return subprocess.run(a, capture_output=True, text=True).stdout
def mem(c, f="memory.current"):
    out = sh("docker", "exec", c, "cat", "/sys/fs/cgroup/" + f).strip()
    return int(out) if out.isdigit() else None
def since(): return round(now() - ev["t0"], 3)

def redis_flood():
    s = socket.create_connection(("127.0.0.1", 16379)); f = s.makefile("rb")
    cmd = b"*3\r\n$5\r\nRPUSH\r\n$5\r\ntasks\r\n$100\r\n" + PAYLOAD + b"\r\n"
    ok = rej = 0; nxt = ev["t0"]
    while now() < ev["stop_at"]:
        s.sendall(cmd * BATCH)
        for _ in range(BATCH):
            line = f.readline()
            if not line:
                ev.update(redis_killed_at=since(), redis_accepted=ok, redis_refused=rej); return
            if line[:1] == b"-":
                rej += 1
                if "redis_full_at" not in ev: ev.update(redis_full_at=since(), redis_error=line.decode().strip())
            else: ok += 1
        ev.update(redis_accepted=ok, redis_refused=rej)
        nxt += BATCH / RATE; d = nxt - now()
        if d > 0: time.sleep(d)

def frame(t, body=b""): return struct.pack(">BBL", 1, t, len(body)) + body
def rx(f, n):
    b = f.read(n)
    if len(b) < n: raise ConnectionError("closed")
    return b

def quicx_flood():
    s = socket.create_connection(("127.0.0.1", 16381)); f = s.makefile("rb")
    sub = frame(1, bytes([10]) + b"send_email" + PAYLOAD)
    ok = rej = 0; nxt = ev["t0"]
    try:
        while now() < ev["stop_at"]:
            s.sendall(sub * BATCH)
            for _ in range(BATCH):
                _, t, n = struct.unpack(">BBL", rx(f, 6)); body = rx(f, n)
                if t == 2: ok += 1
                else:
                    rej += 1
                    if "quicx_full_at" not in ev:
                        ev.update(quicx_full_at=since(), quicx_error=f"0x{body[0]:02x} {body[1:].decode(errors='replace')}")
            ev.update(quicx_accepted=ok, quicx_refused=rej)
            nxt += BATCH / RATE; d = nxt - now()
            if d > 0: time.sleep(d)
    except ConnectionError:
        ev.update(quicx_dropped_at=since(), quicx_accepted=ok, quicx_refused=rej)

def redis_drain():
    s = socket.create_connection(("127.0.0.1", 16379)); f = s.makefile("rb"); n = 0
    while True:
        s.sendall(b"*3\r\n$4\r\nLPOP\r\n$5\r\ntasks\r\n$3\r\n500\r\n")
        head = f.readline()
        if not head.startswith(b"*") or head.startswith(b"*-1") or head.startswith(b"*0"): break
        k = int(head[1:])
        for _ in range(k): f.readline(); f.readline()
        n += k
    ev["redis_drained"] = n

def quicx_queued():
    for line in sh("docker", "exec", Q, "quicx", "status").splitlines():
        if line.strip().startswith("queued"): return int(line.split()[-1].replace(",", ""))
    return -1

def quicx_drain():
    stop = threading.Event()
    def worker():
        s = socket.create_connection(("127.0.0.1", 16381)); f = s.makefile("rb")
        s.sendall(frame(4)); s.settimeout(0.5)          # READY, once
        while not stop.is_set():
            try: _, t, n = struct.unpack(">BBL", rx(f, 6)); body = rx(f, n)
            except Exception: continue
            if t == 7: s.sendall(frame(5, body[:4]))     # TASK -> DONE
        s.close()
    ws = [threading.Thread(target=worker) for _ in range(8)]
    [w.start() for w in ws]
    t0 = now()
    while quicx_queued() != 0 and now() - t0 < 120: time.sleep(0.5)
    time.sleep(1.0); stop.set(); [w.join() for w in ws]
    ev["quicx_drain_seconds"] = round(now() - t0, 1)

time.sleep(1.0)
ev["idle_redis"], ev["idle_quicx"] = mem(R), mem(Q)
ev["t0"] = now(); ev["stop_at"] = ev["t0"] + 60
threads = [threading.Thread(target=redis_flood), threading.Thread(target=quicx_flood)]
[t.start() for t in threads]
while now() < ev["stop_at"]:
    r_done = "redis_full_at" in ev or "redis_killed_at" in ev
    q_done = "quicx_full_at" in ev or "quicx_dropped_at" in ev
    if r_done and q_done: ev["stop_at"] = min(ev["stop_at"], now() + 1.0); break
    time.sleep(0.05)
[t.join() for t in threads]
ev["flood_end"] = since()
ev["peak_redis"], ev["peak_quicx"] = mem(R, "memory.peak"), mem(Q, "memory.peak")
ev["quicx_status_full"] = sh("docker", "exec", Q, "quicx", "status")
if "redis_killed_at" not in ev: redis_drain()
quicx_drain()
time.sleep(10.0)                                         # let both give memory back
ev["after_redis"] = mem(R) if "redis_killed_at" not in ev else None
ev["after_quicx"] = mem(Q)
ev["quicx_status_after"] = sh("docker", "exec", Q, "quicx", "status")
json.dump(ev, open(OUT, "w"), indent=1)
mib = lambda b: None if b is None else round(b / 1048576, 1)
print(json.dumps({k: (mib(v) if k.startswith(("idle_", "peak_", "after_")) else v)
                  for k, v in ev.items() if not k.startswith(("quicx_status", "t0", "stop_at"))}, indent=1))
