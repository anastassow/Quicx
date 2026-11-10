#!/usr/bin/env python3
"""
What a full budget does to valuable work: the real daemon under overload,
one worker slower than the producer, measured for each admission mode.

usage: bench_admission.py QUICXD OUT.json

configs  before   plain SUBMIT (every task level 8), admission = refuse: Quicx 2.0 without values
         refuse   SUBMIT_VALUED, admission = refuse
         oldest   SUBMIT_VALUED, admission = oldest
         value    SUBMIT_VALUED, admission = value
workloads
         steady   2,000 tasks/s for 15 s, each valuable (level 12) with probability 10 %
                  (fixed seed, so every config gets the same tasks), the rest level 4
         burst    valuable tasks at 1,000/s for 3 s, then ordinary ones at 2,000/s for 9 s
The worker takes 2 ms per task (500/s). The budget is 256 KiB: 1,752 tasks of
40 B payload. A task counts as done when its DONE reaches the producer;
evicted when the producer gets FAILED "evicted"; refused on ERROR 0x01.
"""
import collections, json, os, random, socket, statistics, struct, subprocess, sys, tempfile, threading, time

QUICXD, OUT = sys.argv[1], sys.argv[2]
PORT = 16430
WORK_S = 0.002
PAYLOAD = b"f" * 34          # with "frame" as type and the level byte: block of 112 B
VALUABLE, ORDINARY = 12, 4

def frame(t, body=b""): return struct.pack(">BBL", 1, t, len(body)) + body

def rx(f, n):
    b = f.read(n)
    if len(b) < n: raise ConnectionError
    return b

def plan(workload):
    """(time offset, valuable?) for every task, in order"""
    out = []
    if workload == "steady":
        rng = random.Random(2026)                    # the same tasks in every config
        n = 15 * 2000
        for i in range(n): out.append((i / 2000, rng.random() < 0.10))
    else:
        for i in range(3 * 1000): out.append((i / 1000, True))
        for i in range(9 * 2000): out.append((3 + i / 2000, False))
    return out

def run(config, workload):
    mode = "refuse" if config in ("before", "refuse") else config
    conf = tempfile.NamedTemporaryFile("w", suffix=".conf", delete=False)
    conf.write(f"[server]\nport = {PORT}\n\n[memory]\npool_size = 256K\n\n[allocator]\nadmission = {mode}\n"); conf.close()
    d = subprocess.Popen([QUICXD, "start", "--config", conf.name], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    time.sleep(0.4)

    stop = threading.Event()
    def worker():
        s = socket.create_connection(("127.0.0.1", PORT)); f = s.makefile("rb")
        s.sendall(frame(0x04)); s.settimeout(0.2)
        nxt = time.perf_counter()
        while not stop.is_set():
            try: _, t, n = struct.unpack(">BBL", rx(f, 6)); body = rx(f, n)
            except Exception: continue
            if t == 0x07:
                nxt = max(nxt + WORK_S, time.perf_counter())
                while time.perf_counter() < nxt: pass       # 2 ms of "analysis"
                s.sendall(frame(0x05, body[:4]))
        s.close()
    wt = threading.Thread(target=worker); wt.start()

    p = socket.create_connection(("127.0.0.1", PORT)); p.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
    pf = p.makefile("rb")
    pending = collections.deque()                 # (valuable, t_submit) in submit order
    by_id = {}                                    # id -> (valuable, t_submit)
    res = {k: collections.Counter() for k in ("submitted", "accepted", "refused", "done", "evicted")}
    waits = {True: [], False: []}
    reader_done = threading.Event()

    def reader():
        try:
            while True:
                _, t, n = struct.unpack(">BBL", rx(pf, 6)); body = rx(pf, n)
                now = time.perf_counter()
                if t == 0x02:
                    v, ts = pending.popleft(); tid = struct.unpack(">L", body[:4])[0]
                    by_id[tid] = (v, ts); res["accepted"][v] += 1
                elif t == 0x03:
                    v, _ = pending.popleft(); res["refused"][v] += 1
                elif t == 0x05:
                    tid = struct.unpack(">L", body[:4])[0]
                    if tid in by_id:
                        v, ts = by_id.pop(tid); res["done"][v] += 1; waits[v].append(now - ts)
                elif t == 0x06:
                    tid = struct.unpack(">L", body[:4])[0]
                    if tid in by_id:
                        v, _ = by_id.pop(tid); res["evicted"][v] += 1
        except Exception:
            pass
        reader_done.set()
    rt = threading.Thread(target=reader, daemon=True); rt.start()

    t0 = time.perf_counter()
    for off, v in plan(workload):
        delay = t0 + off - time.perf_counter()
        if delay > 0: time.sleep(delay)
        level = VALUABLE if v else ORDINARY
        body = bytes([5]) + b"frame" + PAYLOAD
        msg = frame(0x01, body) if config == "before" else frame(0x0D, bytes([level]) + body)
        pending.append((v, time.perf_counter()))
        res["submitted"][v] += 1
        p.sendall(msg)
    t_end = time.perf_counter()
    # let the worker empty what was accepted (at most 1,752 tasks at 500/s)
    while by_id and time.perf_counter() - t_end < 6: time.sleep(0.05)
    time.sleep(0.3)
    stop.set(); wt.join(); p.close()
    subprocess.run([QUICXD, "stop", "--port", str(PORT)], capture_output=True)
    d.wait(timeout=3); os.unlink(conf.name)

    def pct(a, b): return round(100 * a / b, 1) if b else None
    def q(xs, p): return round(1000 * statistics.quantiles(xs, n=100)[p - 1], 1) if len(xs) > 2 else None
    return {
        "config": config, "workload": workload,
        "valuable_submitted": res["submitted"][True], "ordinary_submitted": res["submitted"][False],
        "valuable_done": res["done"][True], "ordinary_done": res["done"][False],
        "valuable_done_pct": pct(res["done"][True], res["submitted"][True]),
        "valuable_refused": res["refused"][True], "valuable_evicted": res["evicted"][True],
        "ordinary_refused": res["refused"][False], "ordinary_evicted": res["evicted"][False],
        "valuable_wait_ms_p50": q(waits[True], 50), "valuable_wait_ms_p95": q(waits[True], 95),
        "ordinary_wait_ms_p50": q(waits[False], 50),
        "total_done": res["done"][True] + res["done"][False],
    }

results = []
for workload in ("steady", "burst"):
    for config in ("before", "refuse", "oldest", "value"):
        r = run(config, workload); results.append(r)
        print(f"{workload:6} {config:6}  valuable done {r['valuable_done']:5}/{r['valuable_submitted']:5} "
              f"({r['valuable_done_pct']}%)  wait p50 {r['valuable_wait_ms_p50']} ms p95 {r['valuable_wait_ms_p95']} ms  "
              f"valuable refused {r['valuable_refused']} evicted {r['valuable_evicted']}  "
              f"ordinary done {r['ordinary_done']} evicted {r['ordinary_evicted']}  total done {r['total_done']}", flush=True)
json.dump(results, open(OUT, "w"), indent=1)
