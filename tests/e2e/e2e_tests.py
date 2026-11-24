#!/usr/bin/env python3
"""
e2e_tests.py — end-to-end checks against a real quicxd.

    e2e_tests.py DAEMON [PORT]

Every test starts its own daemon and kills it afterwards. Each check prints
one line:  RESULT <test>/<check> PASS|FAIL <detail>

tests
  roundtrip   payloads of 0–1019 B arrive byte-exact (one block per task)
  oversize    a body over 1,024 B gets ERROR 0x03 and the connection closes
  refusal-N   a full queue answers ERROR 0x01 and the connection stays in sync
  liveness-N  after the queue is full, a NEW worker can still connect and drain
              every task (the control pool), for payloads of N bytes
  stale       DONE from a connection that never sent READY must not crash
"""
import random
import struct
import sys
import threading
import time

sys.path.insert(0, __import__("os").path.dirname(__file__))
from qwire import *  # noqa

DAEMON = sys.argv[1]
PORT = int(sys.argv[2]) if len(sys.argv) > 2 else 16391
results = []


def report(name, ok, detail=""):
    results.append(ok)
    print(f"RESULT {name} {'PASS' if ok else 'FAIL'} {detail}".rstrip(), flush=True)


def fresh():
    return Daemon(DAEMON, PORT)


def t_roundtrip():
    d = fresh()
    try:
        w = Conn(PORT)
        w.send(READY)
        time.sleep(0.05)
        p = Conn(PORT)
        rnd = random.Random(1)
        sizes = [0, 1, 15, 16, 17, 100, 500, 960, 1019]
        sent = []
        for i, n in enumerate(sizes):
            typ = b"test"
            payload = bytes(rnd.getrandbits(8) for _ in range(n))
            p.send(SUBMIT, submit_body(typ, payload))
            t, body = p.recv()
            if t != OK:
                report("roundtrip/submit", False, f"size {n}: reply type {t}")
                return
            sent.append((body[:4], typ, payload))
        exact = types_ok = 0
        for tid, typ, payload in sent:
            t, body = w.recv()
            if t != TASK:
                break
            tl = body[4]
            if body[:4] == tid and body[5 + tl:] == payload:
                exact += 1
            if body[5:5 + tl] == typ:
                types_ok += 1
            w.send(DONE, body[:4])
        report("roundtrip/payloads-byte-exact", exact == len(sizes), f"{exact}/{len(sizes)} sizes 0-1019 B")
        report("roundtrip/types-intact", types_ok == len(sizes), f"{types_ok}/{len(sizes)}")
        dones = 0
        try:
            while dones < len(sizes):
                t, body = p.recv(timeout=2)
                if t == DONE:
                    dones += 1
        except Exception:
            pass
        report("roundtrip/done-forwarded-to-producer", dones == len(sizes), f"{dones}/{len(sizes)}")
    finally:
        d.stop()


def t_oversize():
    d = fresh()
    try:
        p = Conn(PORT)
        p.send(SUBMIT, submit_body(b"t", b"x" * 1023))  # body 1,025 B
        t, body = p.recv()
        report("oversize/error-0x03", t == ERROR and body[:1] == bytes([ERR_PAYLOAD_TOO_BIG]),
               f"type {t} code {body[:1].hex()}")
        closed = False
        try:
            p.recv(timeout=1)
        except ConnectionError:
            closed = True
        except Exception:
            closed = False
        report("oversize/connection-closed", closed)
        report("oversize/daemon-alive", d.alive())
    finally:
        d.stop()


def fill_until_refused(p, size):
    payload = b"x" * size
    n = 0
    while n < 500000:
        p.send(SUBMIT, submit_body(b"fill", payload))
        t, body = p.recv()
        if t == OK:
            n += 1
            continue
        return n, t, body
    return n, None, b""


def t_refusal_liveness(size):
    d = fresh()
    try:
        p = Conn(PORT)
        accepted, t, body = fill_until_refused(p, size)
        report(f"refusal-{size}/error-0x01", t == ERROR and body[:1] == bytes([ERR_QUEUE_FULL]),
               f"accepted {accepted}, then type {t} '{body[1:].decode(errors='replace')}'")
        # the same size again: the reply must be a proper frame, so the stream is in sync
        p.send(SUBMIT, submit_body(b"fill", b"x" * size))
        t2, body2 = p.recv()
        p.send(HEARTBEAT)
        t3, _ = p.recv()
        report(f"refusal-{size}/connection-in-sync", t2 in (OK, ERROR) and t3 == PONG,
               f"next submit -> type {t2}, heartbeat -> type {t3}")
        expected = accepted + (1 if t2 == OK else 0)

        # a worker that arrives only now must still get in and drain everything
        drained = 0
        why = ""
        try:
            w = Conn(PORT)
            w.send(READY)
            deadline = time.time() + 60
            while drained < expected and time.time() < deadline:
                tt, bb = w.recv(timeout=3)
                if tt == TASK:
                    drained += 1
                    w.send(DONE, bb[:4])
                elif tt == ERROR:
                    why = f"READY refused: '{bb[1:].decode(errors='replace')}'"
                    break
        except ConnectionError:
            why = "the daemon closed the worker's connection (no memory for it)"
        except Exception as e:  # timeout
            why = f"no task arrived ({type(e).__name__})"
        report(f"liveness-{size}/late-worker-drains-all", drained == expected,
               f"drained {drained}/{expected}" + (f"; {why}" if why else ""))
        # producer side should still work afterwards
        try:
            p.send(SUBMIT, submit_body(b"fill", b"y" * size))
            while True:
                t4, _ = p.recv(timeout=3)
                if t4 in (OK, ERROR):
                    break
            report(f"liveness-{size}/queue-usable-after", t4 == OK, f"type {t4}")
        except Exception as e:
            report(f"liveness-{size}/queue-usable-after", False, type(e).__name__)
        report(f"liveness-{size}/daemon-alive", d.alive())
    finally:
        d.stop()


def t_stale():
    d = fresh()
    try:
        w = Conn(PORT)
        w.send(READY)
        time.sleep(0.05)
        p = Conn(PORT)
        p.send(SUBMIT, submit_body(b"t", b"A" * 16))   # dispatched to w and freed
        time.sleep(0.1)
        c = Conn(PORT)                                  # may reuse the freed block
        time.sleep(0.05)
        c.send(DONE, struct.pack(">I", 1))              # DONE without READY
        time.sleep(0.3)
        report("stale/done-without-ready-no-crash", d.alive(),
               "" if d.alive() else f"exit {d.p.returncode}")
    finally:
        d.stop()


if __name__ == "__main__":
    print(f"daemon {DAEMON} port {PORT}", flush=True)
    t_roundtrip()
    t_oversize()
    for size in (16, 32, 100, 900):
        t_refusal_liveness(size)
    t_stale()
    print(f"SUMMARY passed={sum(results)} failed={len(results) - sum(results)}", flush=True)
