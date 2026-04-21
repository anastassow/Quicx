#!/usr/bin/env python3
"""
bench_quicx.py — Real Quicx benchmark
Measures: throughput (tasks/sec), submit latency (ms), daemon RSS (KB)

Run from Quicx root:
  ./quicxd start --config quicx_bench.conf &
  sleep 0.5
  python3 bench_quicx.py
"""

import socket, struct, time, threading, subprocess, os, sys, statistics

QUICX_HOST   = "127.0.0.1"
QUICX_PORT   = 16381
PROTO_VER    = 1

# Message type constants
MSG_SUBMIT   = 0x01
MSG_OK       = 0x02
MSG_ERROR    = 0x03
MSG_READY    = 0x04
MSG_DONE     = 0x05
MSG_TASK     = 0x07
MSG_WAIT     = 0x08

# ── Benchmark parameters ──────────────────────────────────────
N_WORKERS          = 4
N_PRODUCERS        = 4
TASKS_PER_PRODUCER = 5000          # 20,000 total tasks
PAYLOAD_SIZE       = 64            # payload bytes per task
TASK_TYPE          = b"bench.task"
# ─────────────────────────────────────────────────────────────

def make_frame(msg_type: int, body: bytes = b"") -> bytes:
    """Pack a 6-byte header + body."""
    header = struct.pack(">BBL", PROTO_VER, msg_type, len(body))
    return header + body

def read_exact(sock, n: int) -> bytes:
    buf = b""
    while len(buf) < n:
        chunk = sock.recv(n - len(buf))
        if not chunk:
            raise ConnectionError("socket closed")
        buf += chunk
    return buf

def connect() -> socket.socket:
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
    retries = 20
    while retries:
        try:
            s.connect((QUICX_HOST, QUICX_PORT))
            return s
        except ConnectionRefusedError:
            retries -= 1
            time.sleep(0.15)
    raise RuntimeError("Cannot connect to quicxd on port", QUICX_PORT)

def daemon_rss_kb() -> int:
    """Get quicxd RSS from ps."""
    try:
        out = subprocess.check_output(
            ["ps", "-axo", "pid,rss,comm"],
            text=True
        )
        for line in out.splitlines():
            if "quicxd" in line:
                parts = line.split()
                if len(parts) >= 2:
                    return int(parts[1])
    except Exception:
        pass
    return -1

# ── Worker thread ─────────────────────────────────────────────
stop_workers = threading.Event()
worker_errors = []

def worker_thread():
    try:
        sock = connect()
        # Send MSG_READY (empty body)
        sock.sendall(make_frame(MSG_READY))

        while not stop_workers.is_set():
            sock.settimeout(2.0)
            try:
                hdr = read_exact(sock, 6)
            except socket.timeout:
                continue

            _, msg_type, length = struct.unpack(">BBL", hdr)

            body = read_exact(sock, length) if length > 0 else b""

            if msg_type == MSG_TASK and length >= 4:
                # First 4 bytes of payload = task_id (network byte order)
                task_id_net = body[:4]
                # Send MSG_DONE with same task_id (keep network byte order)
                sock.sendall(make_frame(MSG_DONE, task_id_net))

            elif msg_type == MSG_WAIT:
                pass  # nothing to do, stay connected

        sock.close()
    except Exception as e:
        worker_errors.append(str(e))

# ── Producer thread ───────────────────────────────────────────
producer_latencies = []
producer_lock = threading.Lock()

def producer_thread(n_tasks: int):
    latencies = []
    try:
        sock = connect()

        type_len  = len(TASK_TYPE)
        # MSG_SUBMIT body: [type_len:u8] [type:bytes] [payload:bytes]
        dummy_payload = bytes([0xAB] * PAYLOAD_SIZE)
        body = bytes([type_len]) + TASK_TYPE + dummy_payload

        for _ in range(n_tasks):
            t0 = time.perf_counter()
            sock.sendall(make_frame(MSG_SUBMIT, body))

            # Read response header
            resp_hdr = read_exact(sock, 6)
            _, resp_type, resp_len = struct.unpack(">BBL", resp_hdr)
            if resp_len > 0:
                read_exact(sock, resp_len)   # drain (OK: task_id u32)

            t1 = time.perf_counter()
            latencies.append((t1 - t0) * 1000.0)  # ms

        sock.close()
    except Exception as e:
        pass

    with producer_lock:
        producer_latencies.extend(latencies)


# ═════════════════════════════════════════════════════════════
# MAIN
# ═════════════════════════════════════════════════════════════
if __name__ == "__main__":
    TOTAL_TASKS = N_PRODUCERS * TASKS_PER_PRODUCER

    print()
    print("══════════════════════════════════════════════════════")
    print("  Quicx Benchmark (Python driver)")
    print(f"  {TOTAL_TASKS:,} tasks · {N_WORKERS} workers · {N_PRODUCERS} producers")
    print(f"  Pool: 10 MB · Payload: {PAYLOAD_SIZE} B")
    print("══════════════════════════════════════════════════════")

    # ── Daemon RSS before ─────────────────────────────────────
    rss_before = daemon_rss_kb()
    print(f"\n  Daemon RSS (idle):   {rss_before:,} KB")

    # ── Start workers ─────────────────────────────────────────
    wthreads = []
    for _ in range(N_WORKERS):
        t = threading.Thread(target=worker_thread, daemon=True)
        t.start()
        wthreads.append(t)
    time.sleep(0.4)   # let workers register with daemon

    # ── Run producers ─────────────────────────────────────────
    pthreads = []
    wall_start = time.perf_counter()

    for _ in range(N_PRODUCERS):
        t = threading.Thread(
            target=producer_thread,
            args=(TASKS_PER_PRODUCER,),
            daemon=True,
        )
        t.start()
        pthreads.append(t)

    for t in pthreads:
        t.join()

    wall_end = time.perf_counter()
    stop_workers.set()

    # ── Collect results ───────────────────────────────────────
    rss_after  = daemon_rss_kb()
    elapsed    = wall_end - wall_start
    completed  = len(producer_latencies)
    throughput = completed / elapsed if elapsed > 0 else 0

    avg_lat  = statistics.mean(producer_latencies)  if producer_latencies else 0
    med_lat  = statistics.median(producer_latencies) if producer_latencies else 0
    min_lat  = min(producer_latencies)               if producer_latencies else 0
    max_lat  = max(producer_latencies)               if producer_latencies else 0
    p95_lat  = sorted(producer_latencies)[int(len(producer_latencies)*0.95)] if producer_latencies else 0
    p99_lat  = sorted(producer_latencies)[int(len(producer_latencies)*0.99)] if producer_latencies else 0

    print()
    print("──────────────────────────────────────────────────────")
    print("  RESULTS")
    print("──────────────────────────────────────────────────────")
    print(f"  Tasks submitted   : {completed:,}")
    print(f"  Wall time         : {elapsed:.3f} s")
    print(f"  Throughput        : {throughput:,.0f} tasks/sec")
    print()
    print("  Submit → ACK latency (ms):")
    print(f"    avg : {avg_lat:.3f}")
    print(f"    med : {med_lat:.3f}")
    print(f"    min : {min_lat:.3f}")
    print(f"    max : {max_lat:.3f}")
    print(f"    p95 : {p95_lat:.3f}")
    print(f"    p99 : {p99_lat:.3f}")
    print()
    print(f"  Daemon RSS before : {rss_before:,} KB")
    print(f"  Daemon RSS after  : {rss_after:,} KB")
    print(f"  Daemon RSS Δ      : {rss_after - rss_before:,} KB")
    print("──────────────────────────────────────────────────────")

    # Machine-readable tag for HTML page builder
    print(
        f"\nBENCH_RESULT "
        f"tasks={completed} "
        f"elapsed={elapsed:.3f} "
        f"throughput={throughput:.0f} "
        f"lat_avg={avg_lat:.3f} "
        f"lat_med={med_lat:.3f} "
        f"lat_min={min_lat:.3f} "
        f"lat_max={max_lat:.3f} "
        f"lat_p95={p95_lat:.3f} "
        f"lat_p99={p99_lat:.3f} "
        f"rss_idle={rss_before} "
        f"rss_loaded={rss_after}"
    )
