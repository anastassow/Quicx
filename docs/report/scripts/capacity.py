#!/usr/bin/env python3
"""
capacity.py — how many tasks fit in one daemon, as the traffic changes.

    capacity.py DAEMON FILL PORT phase [phase ...]      phase = payload bytes or "nginx"

One daemon for the whole sequence, default 8 MiB budget. For each phase:
fill with no worker attached until the daemon refuses (the C client `fill`),
measure physical memory, attach 4 workers, drain everything, measure again.

physical memory: phys_footprint on macOS (vmmap), VmRSS on Linux.
Prints one key=value line per phase.
"""
import os
import subprocess
import sys
import threading
import time

sys.path.insert(0, os.path.dirname(__file__))
from qwire import *  # noqa

daemon_bin, fill_bin, port = sys.argv[1], sys.argv[2], int(sys.argv[3])
phases = sys.argv[4:]

done = [0]
lock = threading.Lock()


def worker(stop):
    c = Conn(port)
    c.send(READY)
    while not stop.is_set():
        try:
            t, body = c.recv(timeout=0.2)
        except ConnectionError:
            break
        except Exception:
            continue
        if t == TASK:
            c.send(DONE, body[:4])
            with lock:
                done[0] += 1
    c.close()


d = Daemon(daemon_bin, port)
time.sleep(0.2)
kb, name = physical_kb(d.pid)
print(f"phase=idle measure={name} mem_kb={kb}", flush=True)

for ph in phases:
    args = [fill_bin, "--port", str(port)] + (["--nginx"] if ph == "nginx" else ["--size", ph])
    out = subprocess.run(args, capture_output=True, text=True, timeout=300).stdout.strip()
    acc = int(out.split("accepted=")[1].split()[0])
    refused = out.split('refused_with="')[1].split('"')[0]
    nxt = out.split("next_submit=")[1].split()[0]
    full_kb, _ = physical_kb(d.pid)

    done[0] = 0
    stop = threading.Event()
    ws = [threading.Thread(target=worker, args=(stop,), daemon=True) for _ in range(4)]
    for w in ws:
        w.start()
    want = acc + (1 if nxt == "OK" else 0)        # the probe after the refusal may be accepted
    t0 = time.time()
    while done[0] < want and time.time() - t0 < 120:
        time.sleep(0.05)
    stop.set()
    for w in ws:
        w.join(timeout=2)
    time.sleep(0.5)
    drained_kb, _ = physical_kb(d.pid)
    avg = out.split("payload_avg=")[1].split()[0]
    print(f"phase={ph} accepted={acc} refused_with=\"{refused}\" next_submit={nxt} payload_avg={avg} "
          f"mem_full_kb={full_kb} drained={done[0]}/{want} mem_after_drain_kb={drained_kb}", flush=True)

kb, _ = physical_kb(d.pid)
print(f"phase=end mem_kb={kb} alive={int(d.alive())}", flush=True)
d.stop()
