# quicx benchmark

A real-workload load generator for quicxd: producers submit HTTP access-log
lines, workers parse them, and the harness measures how long the whole round
trip takes.

```
make
./run.sh                        # peak throughput, closed loop
./run.sh -r 30000 -d 30         # 30k tasks/sec, honest latency
./sweep.sh                      # find the capacity knee
```

## What it measures

**End-to-end latency**: from when a task was *due to be sent* until its `DONE`
comes back. That is the number a task queue is judged on. The existing
`bench_quicx.py` measures submit→OK, which times only the daemon's ack and
never touches the worker round trip — it cannot see a slow or starved worker
pool at all.

The clients are C, not Python. A Python client tops out around 50–100k
socket ops/sec and hits the GIL across threads, so it saturates before the
daemon does and you end up measuring the client.

## The workload

`workload.h` generates nginx combined-format log lines:

```
192.168.1.37 - - [15/Aug/2026:19:04:11 +0000] "GET /api/v1/orders/48213 HTTP/1.1" 200 1043 "https://example.com/checkout" "Mozilla/5.0 ..."
```

Payloads land between roughly 120 and 260 bytes. Workers really parse them —
dotted-quad to packed u32, method lookup, FNV-1a over the path, status and
byte counts — and fold the result into per-status totals they print on exit.
Nothing is a `sleep`. Three task types (`log.parse`, `log.enrich`,
`log.audit`) cost different amounts, so the mix is deliberately uneven.

Use `-s N` to add N microseconds of busy work per task if you want to model a
heavier job.

## Open loop vs closed loop

This is the part that decides whether the latency numbers mean anything.

**`--inflight N` (closed loop)** keeps N tasks outstanding and goes as fast as
the daemon allows. Good for peak throughput. **Bad for latency claims** — when
the daemon slows down the client automatically sends less, so the stall never
appears in the numbers. That is coordinated omission.

**`--rate R` (open loop)** puts tasks on a fixed schedule and measures latency
from the *scheduled* time, not the actual send. If the daemon stalls for 5ms,
every task that was due during that window records the wait. p99 will look
worse than closed loop, and it will be true.

Use closed loop for "how many tasks/sec", open loop for "how long does a task
take". Never quote a closed-loop p99.

A quick check that the harness is honest: closed loop with 8 connections × 32
in flight measured 75k tasks/sec at p50 3.6ms. Little's Law says
256 / 75,000 = 3.4ms. It matches, which means the measured latency is real
queueing and not instrument error.

## Reading the output

```
end-to-end latency  (submit due -> DONE received)
  samples      180001   from 2 producer processes

  min            26.1 us
  p50            50.0 us
  p99           120.0 us
  p99.9         182.0 us
  max           354.9 us
```

Each producer writes a raw histogram; `bench_report` merges them before
computing percentiles. Averaging each process's p99 would not give the p99 of
the run.

`sweep.sh` runs a range of offered rates and prints the latency at each. The
useful reading is the **knee** — the first rate where p99 stops tracking p50.
Below it the daemon keeps up; above it the backlog grows for the whole run.
That knee is the capacity number, and moving it right is the only proof a
change helped.

## Trusting a run

A producer exits nonzero if it saw dropped connections or protocol errors, and
`run.sh` prints a warning. **A run with warnings is not a result.** The
per-producer line breaks errors down by code:

```
producer  sent=1252 ok=716 done=11 err=5 dropped_conns=4
          error QUEUE_FULL       x3
          error VERSION_MISMATCH x2
```

Cross-check the worker lines too — they print how many tasks each actually
processed, so an uneven or short-changed worker pool is visible.

## Options

`run.sh`

| flag | meaning | default |
|---|---|---|
| `-w N` | workers | 8 |
| `-p N` | producer processes | 4 |
| `-c N` | connections per process | 4 |
| `-r R` | total tasks/sec, open loop (0 = closed) | 0 |
| `-i N` | closed-loop inflight per connection | 32 |
| `-d S` | duration seconds | 10 |
| `-u S` | warmup seconds, excluded | 2 |
| `-s N` | worker busy-spin microseconds per task | 0 |
| `-P N` | daemon port | 16381 |
| `-f F` | daemon config file | — |

`sweep.sh` takes the same settings via environment: `RATES`, `WORKERS`,
`PRODUCERS`, `CONNS`, `DURATION`, `WARMUP`, `SPIN_US`.

Artifacts land in `results/`: raw histograms, producer and worker logs, the
daemon's own `status` output, and the daemon log.

## Findings from the first runs

**1. Stack buffer overflow in `send_frame_v` — fixed.**
`quicx/src/protocol.c` declared `struct iovec iov[3]` and then wrote the
header plus `part_count` more entries. `protocol_send_task` passes
`part_count = 3` whenever a task has a payload, so every dispatch wrote
`iov[3]` — one past the end — and smashed the stack canary. The daemon
aborted on the first task it handed to a worker. The array needs 4 slots.

**2. Payload allocation failure desynchronizes the connection — open.**
`quicx/src/server.c:159` sends `ERR_QUEUE_FULL` and returns without reading
the payload bytes that are still in the socket. The next read then parses the
middle of a log line as a header, fails the version check, and closes the
connection. The `QUEUE_FULL` → `VERSION_MISMATCH` pair above is that sequence.

This caps the benchmark at roughly 60k tasks/sec — not because the daemon is
slow, but because one payload size class runs dry and the recovery path
destroys the connection. Total pool use at collapse was 10.7%, so it is one
class exhausting, not the pool. The fix is to drain `real_len` bytes before
returning.

**3. `protocol_read_exact` treats `EAGAIN` as fatal — open.**
`quicx/src/protocol.c:8` returns -1 when `read()` sets `EAGAIN`, and sockets
are non-blocking (`server.c:91`). Any message split across TCP segments closes
the connection. Small payloads on loopback usually arrive whole, which is why
it has not bitten yet — over a real network, or once threading changes the
timing, it will. A per-connection read buffer that accumulates bytes and
parses only complete frames is the fix.

`bench_producer` already does exactly that, in `conn_parse`, if you want a
reference implementation.
