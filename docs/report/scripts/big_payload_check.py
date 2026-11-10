#!/usr/bin/env python3
"""
A daemon built with a larger MAX_PAYLOAD: fill an 8 MiB budget with tasks of
one big size, compare the count with the capacity formula, check that a worker
gets the payload byte for byte, that a payload one byte too big is refused,
and that a small task still finds room next to the big ones.

usage: big_payload_check.py QUICXD MAX_PAYLOAD [PORT]
"""
import os, socket, struct, subprocess, sys, time

QUICXD, MAX = sys.argv[1], int(sys.argv[2])
PORT = int(sys.argv[3]) if len(sys.argv) > 3 else 16392
SIZE = MAX - 192                      # a big payload below the limit

def frame(t, b): return struct.pack(">BBI", 1, t, len(b)) + b
def rd(f):
    _, t, n = struct.unpack(">BBI", f.read(6)); return t, f.read(n)

d = subprocess.Popen([QUICXD, "start", "--port", str(PORT)], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
time.sleep(0.4)
ok = True
def check(name, cond):
    global ok
    ok &= bool(cond); print(("PASS" if cond else "FAIL") + ": " + name)
try:
    p = socket.create_connection(("127.0.0.1", PORT)); pf = p.makefile("rb")
    payload = os.urandom(SIZE); n = 0
    while True:
        p.sendall(frame(0x01, bytes([5]) + b"image" + payload)); t, _ = rd(pf)
        if t != 0x02: break
        n += 1
    block = -(-(64 + 6 + SIZE) // 16) * 16            # record + type length, type, payload
    expect = (508 - 508 // 64) * (16384 // block)
    check(f"{SIZE} B tasks in 8 MiB: {n}, formula {expect}", n == expect)
    w = socket.create_connection(("127.0.0.1", PORT)); wf = w.makefile("rb"); w.sendall(frame(0x04, b""))
    t, body = rd(wf); tl = body[4]
    check("worker gets the payload byte for byte", t == 0x07 and body[5 + tl:] == payload)
    p.sendall(frame(0x01, bytes([5]) + b"image" + os.urandom(MAX + 1))); t, body = rd(pf)
    check(f"{MAX + 1} B is refused as too big", t == 0x03 and body[:1] == b"\x03")
    p2 = socket.create_connection(("127.0.0.1", PORT)); p2f = p2.makefile("rb")
    p2.sendall(frame(0x01, bytes([4]) + b"tiny" + b"x" * 100)); t, _ = rd(p2f)
    check("a 100 B task still finds room", t == 0x02)
finally:
    d.terminate(); d.wait()
print("--- big payload checks passed ---" if ok else "--- big payload checks FAILED ---")
sys.exit(0 if ok else 1)
