# Quicx: Threading Design, Measured Results, and Comparison

Working document. Covers the proposed multithreaded architecture, the
benchmark harness built to validate it, the numbers that harness produced, the
bugs it uncovered, and how Quicx sits against real systems and the literature.

**Test machine:** Apple M4 Pro, 12 cores, 24 GB, macOS 26.5.1, arm64,
Apple clang 17.0.0. All numbers below are from this machine, loopback only.

---

## 1. Where things stand

| | |
|---|---|
| current architecture | single thread, one event loop, one `Quicx` |
| measured peak | **75k tasks/sec** (closed loop, 8 connections) |
| measured latency | **p50 51 µs, p99 111 µs** at 40k/s offered |
| measured ceiling | ~60k/s, caused by a **bug**, not by speed |
| worker CPU utilization | **0.13%** — the daemon is entirely syscall-bound |
| bugs found | 3 (1 fixed, 2 open) |

The single most important measurement: **`-O0` and `-O2` builds perform the
same.** Time goes to socket syscalls, not to computation. Optimizing the C will
not move this number. Spreading sockets across threads will.

---

## 2. Current architecture

One thread runs everything:

```
while (1) {
    n = el_wait(loop_fd, events, 64, -1);      /* blocks */
    for each ready fd:
        listener?  -> accept, pmad_alloc a Connection, el_add
        otherwise  -> parse one message, dispatch on message type
}
```

`Quicx` is one struct holding `PMAD` + `Queue` + `WorkerPool` + `Stats`
(`quicx/include/quicx.h`). Because there is one thread, none of those need
locks, every allocation and free happens on the same thread, and every socket
is read and written by the same thread.

A task's life: allocated at `MSG_SUBMIT` (`daemon.c:16`), pushed to the queue,
and `try_dispach` runs immediately (`daemon.c:38`). If a worker is idle the
task is handed over and freed within microseconds (`dispacher.c:24-25`). The
queue is only non-empty when no worker is free.

Connection type is *not* what drives handling — `handle_readable` switches on
the **message** type. `conn->type` is read in exactly one place,
`daemon_handle_disconnect`.

---

## 3. Proposed threading design

### The invariant

**A thread only touches its own memory and its own sockets.** Threads exchange
messages; they never reach into each other.

Two consequences, both forced rather than chosen:

- **A Task is always allocated from the pool that will free it.** PMAD has no
  locking. A task allocated on thread 1 and freed on thread 3 is heap
  corruption, not a race you can tune away.
- **An fd is read *and written* by exactly one thread.** `protocol_read_exact`
  assembles one message from several `read()` calls. Two threads reading the
  same socket shred the byte stream, and there are no record boundaries to
  resynchronize on.

### Anatomy of one thread

```
┌─ Thread N ──────────────────────────────────────────┐
│  event loop (kqueue/epoll)                          │
│    ├── wake_fd[0]     ← other threads poke here     │
│    ├── producer fds      this thread reads AND      │
│    └── worker fds        writes only these, ever    │
│                                                     │
│  Quicx  ── private, nobody else touches it          │
│    ├── PMAD      alloc + free, same thread only     │
│    ├── Queue                                        │
│    ├── WorkerPool                                   │
│    └── Stats                                        │
│                                                     │
│  Inbox  ── MPSC: others push, only N pops           │
│  published stats ── atomics, own cache line         │
└─────────────────────────────────────────────────────┘
```

N identical threads. Not one acceptor plus N workers — every thread accepts,
parses, queues, dispatches and replies.

### Placement: local-first with spill

```c
case MSG_SUBMIT: {
    uint32_t id = atomic_fetch_add(&rt->next_task_id, 1);

    if (me->quicx.pool.idle_count > 0 && me->mem_used < me->spill_watermark) {
        /* FAST PATH — today's code, unchanged */
        Task* t = pmad_alloc(&me->quicx.pmad, sizeof(Task));
        queue_push(&me->quicx.queue, t);
        try_dispach(&me->quicx);
    } else {
        /* SPILL — the task is never created locally */
        int dst = route(rt);
        inbox_push(&rt->threads[dst].inbox, &m);   /* raw bytes */
        wake(dst);
    }
    protocol_send_ok(conn->fd, id);
}
```

Why this shape:

- The fast path is **code that already exists and is already tested**. One
  comparison in front of it. All new machinery engages only under saturation.
- `idle_count` is thread-local: a plain load, no atomics, no cross-core
  traffic, and it cannot be stale. Every remote figure is an approximation.
- Greedy-local is not a heuristic here, it is optimal. If a local worker is
  idle, a remote hop costs a copy, a pipe write and a ~1–5 µs thread wake, then
  does the same dispatch. There is no "faster worker" in the model.

**Critically: routing is per task, not per connection.** One producer sending
1000 tasks can have them land on 1000 different threads. The connection is
pinned to a thread only for byte parsing.

### The reply path

A task may finish on a thread that does not own the producer's socket, so the
worker must remember where to answer. Both structs have spare padding, so this
costs zero bytes:

```c
/* queue.h:23 — _pad[2] */    uint8_t origin;          /* owns producer_fd */
/* workers.h:20 — _pad[3] */  uint8_t current_origin;
```

Local tasks reply with a direct write (today's path). Spilled tasks post
`IN_REPLY` to the origin thread's inbox.

### The hazard that will bite

fd reuse. Thread 3 holds `current_fd = 42`; the producer disconnects; thread 0
closes 42 and accepts a new connection, which the kernel assigns **42 again**.
Thread 3's reply is then delivered to an unrelated client.

Fix with a per-thread generation table, which keeps `Connection` at 16 bytes so
the existing `_Static_assert` still holds:

```c
uint32_t gen[MAX_CONNECTIONS];      /* 4 KB; MAX_CONNECTIONS is already 1024 */
me->gen[conn->fd]++;                /* in conn_close, before close() */
if (me->gen[r.fd] != r.conn_gen) continue;   /* stale reply — drop it */
```

### Build order

1. `Runtime` + `Thread` with `thread_count = 1`. Split `server_start` into
   listener setup plus a per-thread loop. Everything still works. Most of the
   diff, none of the risk.
2. Inbox + self-pipe, still single-threaded. Have the thread post to itself and
   watch the message come back out — the whole path is testable without
   concurrency. **Skipping this step is what makes the first real bug
   undebuggable.**
3. Spawn N threads, route round-robin. Now genuinely parallel.
4. Replace round-robin with a real policy.

---

## 4. Alternatives considered and rejected

| idea | why not |
|---|---|
| **Shard everything** (connection pinned to a queue for its whole life) | Splits the worker pool. Tasks from connection A can only reach workers on thread A. Rejected in favour of per-task routing. |
| **Shared queue + one mutex, threaded I/O only** | Perfect balance, but dispatch writes to sockets owned by other threads, so fd lifetime becomes shared. Trades a *balance* problem for a *correctness* problem — use-after-close plus fd reuse delivering payloads to the wrong client. |
| **Single entry point, cascade overflow** | Quicx performs no computation; its entire job is socket I/O. Putting all connections on one thread keeps all of that on one core. The entry point is precisely what must be parallel. |
| **Steal from the back of the fullest queue** | Produces the same placement as spilling early but pays extra: a local alloc, a local free, a copy, a message, plus the latency already spent queued. See §9 for where this judgement needed revising. |
| **Dedicated ML router thread** | `route()` costs ~20 ns; a cross-thread round trip costs 1–5 µs and model inference another 1–10 µs. 50–250× the cost of the decision it replaces, and the answer arrives after the task is gone. Worker availability is *directly observable*, so there is no hidden structure for a model to learn. |

**Where ML does belong:** offline tuning of PMAD size classes. `sizes[]` and
`percentages[]` in `config.h` are hand-picked; `pmad_get_stats` already
collects exact per-class allocation data. Offline optimization over a workload
trace, zero hot-path cost, and it improves the property Quicx is actually
distinctive for. "Hand-tuned config wastes X%, learned config wastes Y%" is a
real result. "We used ML to pick a queue" is not, because a two-line rule
matches it.

---

## 5. The benchmark harness

Lives in `bench/`. Build with `make`, run with `./run.sh`, sweep with
`./sweep.sh`.

| file | purpose |
|---|---|
| `bench_producer.c` | load generator + latency recorder, N connections on one poll loop |
| `bench_worker.c` | worker that genuinely parses the payload |
| `workload.h` | nginx access-log generation and parsing |
| `bench_report.c` | merges histograms, prints percentiles |
| `run.sh` | one full run: daemon, workers, producers, report, cleanup |
| `sweep.sh` | rate sweep to locate the capacity knee |

### What it measures and why that differs from the old harness

**End-to-end latency**: from when a task was *due to be sent* until its `DONE`
returns. The pre-existing `bench_quicx.py` measures submit→OK, which times only
the daemon's ack and never touches the worker round trip — it cannot observe a
starved or slow worker pool at all. It is also Python, which tops out around
50–100k socket ops/sec and hits the GIL across threads, so it saturates before
the daemon does and ends up measuring itself.

### Open loop vs closed loop

- **`--inflight N` (closed loop)** — keeps N outstanding, runs as fast as the
  daemon allows. Correct for peak throughput. **Invalid for latency claims**:
  when the daemon slows, the client automatically sends less, so the stall
  never appears. That is coordinated omission.
- **`--rate R` (open loop)** — tasks are due on a fixed schedule and latency is
  measured from the *scheduled* time. A 5 ms stall is charged to every task due
  during it. p99 looks worse and is true.

### Harness validation

Closed loop, 8 connections × 32 in flight = 256 outstanding, measured 75k
tasks/sec at p50 3.6 ms. Little's Law: 256 / 75,000 = **3.4 ms**. The match
means the measured latency is real queueing, not instrument error.

### The workload

Real nginx combined-format lines, 120–260 bytes:

```
192.168.1.37 - - [15/Aug/2026:19:04:11 +0000] "GET /api/v1/orders/48213 HTTP/1.1" 200 1043 "https://example.com/checkout" "Mozilla/5.0 ..."
```

Workers actually parse them — dotted-quad to packed u32, method lookup, FNV-1a
over the path, status and byte extraction — and keep per-status totals printed
on exit, so a run can be verified rather than trusted. Nothing is a `sleep`.

---

## 6. Measured results

### Rate sweep (8 workers, 8 connections, 6s, 2s warmup)

| offered | p50 | p99 | p99.9 | max | errors |
|---:|---:|---:|---:|---:|---:|
| 5k/s | 102 µs | 149 µs | 189 µs | 628 µs | 0 |
| 10k/s | 52 µs | 116 µs | 157 µs | 229 µs | 0 |
| 20k/s | 52 µs | 99 µs | 163 µs | 278 µs | 0 |
| 40k/s | 56 µs | 118 µs | 185 µs | 428 µs | 0 |
| 60k/s | 42 µs | 93 µs | 156 µs | 308 µs | 12 · **connections dropped** |
| 80k/s+ | — | — | — | — | run collapses |

Latency is flat from 10k to 40k — the daemon is comfortably keeping up. The
wall at 60k is not saturation, it is the bug in §7.2.

The elevated p50 at 5k/s is a real effect, not noise: at low rates the socket
and the daemon's caches go cold between tasks.

### Closed loop peak

8 workers, 8 connections, 32 in flight each:

```
596,411 tasks over the 8s run, 449,823 inside the 6s measured window
75.0k tasks/sec          0 errors, 0 dropped connections
p50 3.60 ms   p99 3.89 ms   max 4.16 ms
workers processed 73,904 – 75,068 tasks each (evenly balanced)
```

The millisecond latencies here are the closed loop doing its job, not a
regression: 256 outstanding requests divided by 75k/s is 3.4 ms of pure
queueing. This is the number to quote for throughput and the wrong number to
quote for latency.

### Optimization level makes no difference

| build | 20k/s p50 | 20k/s p99 | 40k/s p50 | 40k/s p99 |
|---|---:|---:|---:|---:|
| `-O0` (current Makefile) | 52 µs | 97 µs | 50 µs | 111 µs |
| `-O2` | 48 µs | 110 µs | 51 µs | 112 µs |

Within run-to-run noise. **The Makefile currently builds `quicxd` with
`CFLAGS_DEBUG = -g` and no `-O` flag at all** — worth fixing for releases, but
it is not costing throughput.

### Run-to-run variance

Same binary, same 40k/s rate, three consecutive runs:

```
p50=48.0  p90=66.0  p99=113.0  p999=174.0  max=294.0
p50=51.0  p90=68.0  p99=112.0  p999=174.0  max=352.0
p50=51.0  p90=67.0  p99=110.0  p999=163.0  max=308.0
```

Stable. A single run can still catch an outlier — one 40k/s run showed a 2 ms
p99 that three repeats did not reproduce — so **repeat before believing any
single number.**

### Where the time goes

Workers were busy **5.2 ms out of 4 seconds — 0.13%**. Parsing a log line costs
about 170 ns. Everything else is socket round trips.

This is the strongest single argument for the threading design: there is
essentially no computation to parallelize, so the only thing worth spreading
across cores is socket handling.

---

## 7. Bugs found

### 7.1 Stack buffer overflow in `send_frame_v` — FIXED

`protocol.c:36` declared `struct iovec iov[3]` and then wrote the header plus
`part_count` more entries:

```c
struct iovec iov[3];
iov[0].iov_base = &hdr;
for (int i = 0; i < part_count; i++)
    iov[i + 1] = parts[i];          /* part_count == 3 writes iov[3] */
```

`protocol_send_task` passes `count = 3` whenever a task has a payload
(`protocol.c:153`), so **every task dispatch** wrote one `struct iovec` past
the end and smashed the stack canary. The task was sent, the function returned,
and the process aborted.

The daemon died on the first task it ever handed to a worker. The Python
harness never caught it because it only exercised the submit→OK path.

Fixed: the array needs 4 slots, plus a guard on `part_count`.

### 7.2 Payload allocation failure desynchronizes the connection — OPEN

`server.c:159` sends `ERR_QUEUE_FULL` and returns **without consuming the
payload bytes still sitting in the socket**:

```c
payload_buf = pmad_alloc(&quicx->pmad, real_len);
if (!payload_buf) {
    protocol_send_error(conn->fd, ERR_QUEUE_FULL, "out of memory");
    return;                       /* real_len bytes still unread */
}
```

The next read then parses the middle of a log line as a header, fails the
version check, and closes the connection. The producer's error breakdown shows
exactly that sequence:

```
error QUEUE_FULL       x3
error VERSION_MISMATCH x2
```

`VERSION_MISMATCH` can only come from garbage in the stream.

**Total pool usage at collapse was 10.7%** — a single payload size class ran
dry, not the pool. This is what caps the benchmark at ~60k/s. Fix: drain
`real_len` bytes before returning.

### 7.3 `protocol_read_exact` treats `EAGAIN` as fatal — OPEN

`protocol.c:8` returns -1 when `read()` sets `EAGAIN`, and sockets are
non-blocking (`server.c:91`). Any message split across TCP segments closes the
connection. Small payloads on loopback usually arrive whole, which is why it
has not bitten yet — over a real network, or once threading changes the timing,
it will.

Fix: a per-connection read buffer that accumulates bytes and parses only
complete frames. `bench_producer`'s `conn_parse` is a working reference.

---

## 8. Comparison with real systems

| system | what it is | reported performance |
|---|---|---|
| **Kafka** | distributed log | >1M msg/s sustained, batched, disk-backed, ~ms latency |
| **RabbitMQ** | AMQP broker | peaks ~40k msg/s |
| **NATS** | lightweight pub/sub | sub-millisecond latency, small footprint |
| **Redis** as queue | `LPUSH`/`BRPOP` | ~161k `LPUSH`/s, p50 0.303 ms; ~845k/s pipelined over unix sockets |
| **beanstalkd** | dedicated job queue, C, single-threaded event loop | closest architectural twin; lower throughput than Redis, lower wait times |
| **Quicx** | this | 75k tasks/s; p50 51 µs, p99 111 µs @ 40k/s |

### The comparison that is easy to get wrong

**A Quicx "task" is not a Redis "operation."** One Quicx task is four socket
traversals — producer→daemon, daemon→worker, worker→daemon, daemon→producer.
One Redis `LPUSH` is a single round trip, and a real Redis job queue needs
`LPUSH` plus `BRPOP` plus an ack.

So 75k tasks/sec is roughly **300k socket messages/sec**, which puts Quicx in
the same league as Redis's 161k `LPUSH`/sec. And the 51 µs figure is a
*complete job lifecycle*, where Redis's 0.303 ms is one push.

Quoting "75k/s" against "161k/s" would understate Quicx by roughly 4×.

**Caveats that belong in any writeup:** those third-party numbers are from
unknown hardware and are ballpark only; and beanstalkd, Redis and NATS all
provide durability, retries and reconnection semantics that Quicx does not. If
Quicx wins on latency, part of that margin is work it is not doing.

### Where Quicx has no real competitor

None of the above will tell you "I will use exactly 8 MB, and here is the
per-size-class breakdown." Bounded, predictable memory with honest backpressure
is the genuine differentiator — a positioning claim, not a speed claim.

That also means the exact-memory signal belongs in **admission control**, not
routing: it says *when to refuse work*, not *where to send it*.

---

## 9. Literature

**[The Power of Two Choices in Randomized Load Balancing](https://www.eecs.harvard.edu/~michaelm/postscripts/tpds2001.pdf)** —
Mitzenmacher, IEEE TPDS 2001. Two random choices drop max load from O(log n) to
O(log log n) — exponential improvement. Three choices is only a *constant*
better than two. Sample two threads; scanning all N is wasted work.

**[Join-Idle-Queue](https://www.microsoft.com/en-us/research/publication/join-idle-queue-a-novel-load-balancing-algorithm-for-dynamically-scalable-web-services/)** —
Lu et al., Performance Evaluation 2011. Routes to *idle* servers with **zero
communication at job arrival** — idle servers announce themselves rather than
dispatchers probing. Beats power-of-two. Quicx's local-first `idle_count` check
is the single-dispatcher case of this.

**[Sparrow](https://people.eecs.berkeley.edu/~matei/papers/2013/sosp_sparrow.pdf)** —
Ousterhout et al., SOSP 2013. Batch sampling plus **late binding**: probes go
out early, but a task is not committed until a worker asks for it. Within 12%
of an ideal scheduler. This is the correct form of the "decide early, commit
late" instinct.

**[ZygOS](https://marioskogias.github.io/docs/zygos.pdf)** — Prekas et al.,
SOSP 2017. **This one revises the judgement in §4 on work stealing.**

| service time distribution | partitioned vs shared queue |
|---|---|
| fixed | roughly equal |
| exponential (CV = 1.0) | shared queue ~**2× better** at p99 |
| bimodal | shared queue **3–5× better** |

Partitioned per-core queues suffer head-of-line blocking: one slow task blocks
everything behind it while other cores idle. ZygOS recovers most of the loss
with work stealing, reaching 75% of theoretical ideal for 10 µs tasks and 88%
for 25 µs tasks.

So the argument "queues are almost always empty, so stealing has nothing to
move" holds precisely in the **uniform service time** regime — which is also
the regime where ZygOS finds partitioning fine. Once task durations vary, the
queues are *not* empty, head-of-line blocking is real, and stealing is worth
2–5× at the tail.

**[Shinjuku](http://www.abelay.me/data/shinjuku_nsdi19.pdf)** — Kaffes et al.,
NSDI 2019. Goes further with preemption, for the same reason.

---

## 10. Open items

**The benchmark cannot currently answer the architecture question.** The
log-parsing workload has near-uniform service time — every worker recorded
5.2 ms busy within a few percent of the others. That is exactly the regime
where ZygOS says partitioned and shared queues perform the same. To measure
whether sharding or sharing wins, the worker needs **variable service time**
(exponential and bimodal), keyed off the `type` field that already exists.

Ordered next steps:

1. Fix the `QUEUE_FULL` desync (§7.2) so the sweep can measure past 60k/s.
2. Fix the `EAGAIN` handling (§7.3) *before* threading — partial reads get more
   likely once timing changes, and debugging it with N threads is much harder.
3. Add variable service-time distributions to `bench_worker`.
4. Re-run the sweep to establish a clean single-threaded baseline.
5. Build threading stage 1 (`Runtime`/`Thread` split at `thread_count = 1`).
6. Stages 2–4, re-running the sweep after each.

The knee moving right is the only proof that any of it helped.
