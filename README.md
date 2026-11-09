<p align="center">
  <picture>
    <source media="(prefers-color-scheme: dark)" srcset="docs/assets/banner-dark.png">
    <img src="docs/assets/banner-light.png" alt="Quicx — the task queue with a hard memory limit" width="100%">
  </picture>
</p>

<p align="center">
  <a href="https://quicx.dev"><img alt="quicx.dev" src="https://img.shields.io/badge/quicx.dev-website-ff5700?style=flat-square&labelColor=0a171d"></a>
  <img alt="version 2.0.0" src="https://img.shields.io/badge/version-2.0.0-ff5700?style=flat-square&labelColor=0a171d">
  <img alt="C17, no dependencies" src="https://img.shields.io/badge/C17-no%20dependencies-4b5f68?style=flat-square&labelColor=0a171d">
  <img alt="macOS and Linux" src="https://img.shields.io/badge/macOS%20%7C%20Linux-kqueue%20%7C%20epoll-4b5f68?style=flat-square&labelColor=0a171d">
  <a href="LICENSE"><img alt="MIT licence" src="https://img.shields.io/badge/licence-MIT-4b5f68?style=flat-square&labelColor=0a171d"></a>
</p>

**Quicx is a task queue daemon written in C that never uses more memory than you give it.**

Your applications hand it work — send an email, resize an image, parse a log line — and your workers take that work one task at a time. Quicx sits in between: it accepts tasks over TCP, keeps them in order, gives each one to the next free worker, and tells the producer when the work is done. The whole daemon is one 55 KB binary, one process and one thread, with no runtime dependencies.

Its defining promise is memory. You give Quicx a budget when it starts (8 MiB by default). It maps that budget once and never asks the operating system for another byte. When the budget is full, a new task gets a clean "queue full" answer instead of making the process grow. Inside the budget, **apool**, Quicx's own allocator, gives memory to whatever sizes of task are arriving, so the default 8 MiB holds **46,593** tasks of 100 B. It also gives memory back after a burst.

[Website](https://quicx.dev) · [Documentation](https://quicx.dev/docs) · [Benchmarks](#benchmarks) · [Papers](#papers-and-references) · [Report (PDF)](docs/report/main.pdf)

---

## Contents

1. [What Quicx is](#what-quicx-is)
2. [How it works](#how-it-works)
3. [Memory: the apool allocator](#memory-the-apool-allocator)
4. [Benchmarks](#benchmarks)
5. [Install](#install)
6. [Quick start](#quick-start)
7. [Using Quicx from your code](#using-quicx-from-your-code)
8. [Configuration](#configuration)
9. [Wire protocol](#wire-protocol)
10. [Limits](#limits)
11. [Building and testing](#building-and-testing)
12. [Repository](#repository)
13. [Compared with v1.0.4](#compared-with-v104)
14. [Papers and references](#papers-and-references)

## What Quicx is

Most brokers grow with their load: more tasks waiting means more memory, until the machine or a limit says no. That is fine on a dedicated server and a problem everywhere else — next to your backend on a small VM, on an edge box, in a container with a hard limit. Quicx turns the question around. You decide how much memory the queue may ever use, and it lives inside that number.

What you get:

- **A hard memory limit.** One budget, mapped once at start-up and never exceeded. When it is full, `SUBMIT` gets `ERROR 0x01` (queue full) and the connection stays usable.
- **Memory that follows the traffic.** 16 KiB pages go to the task sizes that are actually arriving and come back when they empty; after a burst, their memory goes back to the operating system.
- **Speed without tuning.** 81,900 tasks per second end to end on one thread, 49 µs median from submit to done (Apple M4 Pro).
- **One small binary.** 55 KB, written in C17, no dependencies, for macOS and Linux on x86-64 and arm64.
- **A tiny binary protocol.** A 6-byte header and 12 message types. Any language can speak it; a Java client is ready-made.
- **Live visibility.** `quicx status` shows tasks, workers and every page in use, read from the daemon over a side channel.

Quicx is a good fit when tasks are small (up to 1 KB), local and cheap to redo from their source, and a fixed memory ceiling matters more than durable delivery. It is not a good fit when a task must survive a worker crash or a restart: see [Limits](#limits).

## How it works

<picture>
  <source media="(prefers-color-scheme: dark)" srcset="docs/report/figures/quicx-architecture-dark.png">
  <img src="docs/report/figures/quicx-architecture-light.png" alt="Quicx component diagram: producers and workers talk to quicxd over TCP; inside, an event loop and the protocol feed the server, daemon, queue and dispatcher; every task lives in one block of the task pool from SUBMIT until it is sent as TASK." width="100%">
</picture>

One task, step by step (the orange path above):

1. A producer sends **SUBMIT** with a task type and a payload of up to 1,024 B. The event loop wakes up for it.
2. The server takes **one block** from the task pool — a 64 B record with the payload right behind it — and reads the payload straight into place. If the budget is full, the producer gets `ERROR 0x01`.
3. The daemon **queues** the task (one FIFO) and answers **OK** with the task id.
4. The dispatcher **pops** the oldest task as soon as a worker has said **READY**.
5. The task goes to that worker as **TASK**.
6. Its block is **freed** at once, so delivery is at-most-once. The worker's **DONE** or **FAILED** is passed on to the producer.

| Component | File | Job |
|---|---|---|
| Event loop | `event_loop.c` | one thread waits on every socket with kqueue (macOS) or epoll (Linux) |
| Protocol | `protocol.c` | reads and writes frames: 6-byte header, body |
| Server | `server.c` | accepts connections, reads and checks frames, allocates each task's block |
| Daemon | `daemon.c` | acts on every message: queue a task, register a worker, pass on results |
| Queue | `queue.c` | one FIFO of waiting tasks |
| Dispatcher | `dispacher.c` | gives the oldest task to an idle worker and frees its block |
| Worker pool | `workers.c` | idle and busy workers |
| Stats | `stats.c` | counters for `quicx status` |
| apool | `apool.c` | the allocator: the task pool and the control pool |
| Config | `config.c`, `pool_config.c` | reads `quicx.conf` |
| CLI | `cli.c` | `quicx start`, `stop`, `status`, `version` |

Connections and workers are kept in their own small **control pool** (3 pages), so a queue full of tasks can never stop a worker from registering and draining it.

## Memory: the apool allocator

<picture>
  <source media="(prefers-color-scheme: dark)" srcset="docs/report/figures/apool-architecture-dark.png">
  <img src="docs/report/figures/apool-architecture-light.png" alt="apool v2: one mapping of 16 KiB pages and 32 B page records; size classes own lists of pages; freed blocks inside a page form an encoded free list; empty pages stay warm, then give their memory back." width="100%">
</picture>

The budget is one mapping: 508 pages of 16 KiB, followed by one 32-byte record per page. A page that is in use belongs to one block size (68 sizes, 16 to 1,088 B) and holds blocks of only that size. Blocks carry no header: a block's page is its offset divided by 16 KiB, and the page's record says everything else. Inside a page, freed blocks form a list through their own first bytes, and blocks never used yet come from a bump offset, so allocating and freeing are O(1). When the last block of a page is freed, the page goes back to a shared list where **any** size can take it next. That is how the memory follows the traffic.

Every idea in apool comes from a published allocator, and each one sits behind its own switch so its effect could be measured on its own. These are the ones Quicx ships with:

| Feature | Idea from | What it buys (measured) | What it costs (measured) |
|---|---|---|---|
| **Pages move between sizes** | slab allocator [[1]](#ref-1), mimalloc pages [[3]](#ref-3), memcached [[8]](#ref-8) | 46,593 tasks of 100 B in 8 MiB; the pool follows size changes without a restart | — |
| **Reserve** | Linux zone watermarks [[6]](#ref-6), memcached [[8]](#ref-8) | after a flood of one size, a new size still gets room (16 tasks of 900 B instead of 0) | 1.4 % of capacity at 100 B |
| **Hardening** | mimalloc secure mode [[3]](#ref-3) | double frees and overwritten free-list links are caught instead of corrupting memory | 1 ns per task (M4 Pro), 13 ns (i3) |
| **Purge with a warm list** | jemalloc [[4]](#ref-4), mimalloc [[3]](#ref-3), `madvise` [[9]](#ref-9) [[10]](#ref-10) | memory goes back after a burst: 1,104 KB held instead of 8,096 KB | only on bursts that fill and empty the whole pool: 102 instead of 8.7 ns per block |
| **One block per task** | Quicx | one allocation and one free per task: 10 ns less per task on the M4 Pro, 48 ns on the i3 | — |
| **Control pool** | the reserved-memory principle of Linux `mempool` [[7]](#ref-7) | a full task pool can never lock workers out | 3 of the budget's pages |

Two more ideas were measured and are not used by the daemon. Fullest-page-first (Hoard [[2]](#ref-2)) gave no gain on queue traffic. Lifetime classes (LLAMA [[5]](#ref-5)) only help when a caller marks long-lived tasks, which the daemon does not do yet. Every switch is in [`quicx.conf`](#configuration).

## Benchmarks

<picture>
  <source media="(prefers-color-scheme: dark)" srcset="docs/assets/benchmarks-dark.png">
  <img src="docs/assets/benchmarks-light.png" alt="Quicx 2.0.0 measured: 81,900 tasks per second, 49 µs p50 latency, 46,593 tasks of 100 B in 8 MiB and 2,080 KB held after a burst on an Apple M4 Pro; latency, capacity and memory charts for the M4 Pro and an Intel i3-2100." width="100%">
</picture>

Quicx 2.0.0 with its default settings (the features above), built with `-O2`, on two very different machines:

| | Mac | Linux PC |
|---|---|---|
| CPU | Apple M4 Pro, 12 cores | Intel Core i3-2100 (2011), 2 cores / 4 threads |
| OS, compiler | macOS 26.5, Apple clang 17 | Ubuntu 24.04, gcc 13.3 |

**Throughput and latency** (median of 5 runs)

| | Mac | Linux PC |
|---|---:|---:|
| throughput, tasks/s | **81,900** | 19,000 |
| latency p50 | **49 µs** | 123 µs |
| latency p90 | 64 µs | 157 µs |
| latency p99 | 98 µs | 198 µs |
| latency p99.9 | 143 µs | 208 µs |

Throughput is a closed loop: 8 workers, 2 producers with 4 connections each and 32 tasks in flight per connection, for 10 s, with 0 errors. Latency is the time from submit to done, not to the acknowledgement. It is measured in an open loop at a fixed 40,000 tasks/s on the Mac and 9,000 on the Linux PC, timed from each task's scheduled send so stalls are not hidden. On the Linux PC, eleven processes share 4 hardware threads, so its CPU is the limit.

**Capacity: tasks accepted in the default 8 MiB before "queue full"** (the same on both machines)

| Payload | Tasks |
|---|---:|
| 40 B | 73,146 |
| 100 B | **46,593** |
| nginx log lines (average 188 B) | 30,947 |
| 300 B | 22,044 |
| 900 B | 8,016 |

The sizes are sent one after another to the same daemon and the queue is drained in between, so each size in turn gets nearly the whole budget.

**Memory over one burst: physical memory of the daemon process, KB** (median of 3 runs)

| | Mac | Linux PC |
|---|---:|---:|
| idle, just started | 1,056 | 1,796 |
| queue full | 9,088 | 9,816 |
| after the burst drains | **2,080** | **2,736** |

macOS measures `phys_footprint`, Linux resident set size. Under the throughput load, the most Quicx used was 2.8 % of its pool (14 of 508 pages, sampled ten times a second).

**Footprint:** a 54.9 KB binary (`-O2`, nothing stripped), about 2,800 lines of C, one thread, no runtime dependencies. Allocation alone takes 14.8 ns per task (alloc and free) on the M4 Pro and 72.8 ns on the i3.

Every number comes from the raw output in [`docs/report/results`](docs/report/results), produced by `docs/report/scripts/run_all.sh` with C clients over loopback TCP. The [report](docs/report/main.pdf) describes each benchmark and its limits.

## Install

```sh
curl -fsSL https://quicx.dev/install.sh | sh
```

The installer picks the binary for your system (macOS or Linux, x86-64 or arm64) and puts `quicx` into `/usr/local/bin`. To build it yourself, see [Building and testing](#building-and-testing).

## Quick start

```sh
quicx start                          # 8 MiB budget, port 16381
quicx start --config quicx.conf      # or with a config file
quicx status                         # tasks, workers and memory, live
quicx stop
```

On start, Quicx prints what it is running with:

```
quicx v2.0.0 starting
  port:    16381
  pool:    8388608 bytes, 16 KiB pages given to block sizes on demand
  apool:   reserve, hardening, purge
  pages:   508 for tasks (63 kept warm, 7 reserved), 3 for connections and workers
```

`quicx status` with 1,200 tasks of 100 B and 300 of 900 B waiting:

```
  quicx v2.0.0  ·  pid 8278  ·  up 0h 0m 00s

  tasks     submitted       1,500
            completed           0   ░░░░░░░░░░░░░░░░░░░░     0.0%
            failed              0                            0.0%
            in flight       1,500   = 1500 queued + 0 running
            queued          1,500

  workers   busy                0   ░░░░░░░░░░░░░░░░░░░░     0.0%
            idle                0
            total               0   · 0 registered since start

  memory    32 / 508 pages          █░░░░░░░░░░░░░░░░░░░     6.3%
            pool 7.95 MiB, 16 KiB pages · 2 sizes in use

           176B    1,200 /   1,209  ███████████████████░    99.3%    13 pages
           976B      300 /     304  ███████████████████░    98.7%    19 pages
```

## Using Quicx from your code

Producers and workers are plain TCP clients. With the Java client (`dev.quicx:quicx-client`):

```java
// a producer: submit a task and get its id back
try (QuicxClient client = new QuicxClient("localhost", 16381)) {
    int taskId = client.submit("send_email", "{\"to\":\"user@example.com\"}");
}

// a worker: take tasks of one type, one at a time
new QuicxWorker("localhost", 16381)
    .handle("send_email", payload -> { /* do the work */ })
    .start();
```

Any other language can speak the [wire protocol](#wire-protocol) directly; the full format of every message is in the [protocol documentation](https://quicx.dev/docs/binary-protocol).

## Configuration

Every key is optional, and the values below are the built-in defaults. [`quicx.conf`](quicx.conf) in this repository is exactly this file:

```ini
[server]
port = 16381

[memory]
pool_size = 8M          # the whole budget, never exceeded (K, M, G)

[allocator]
reserve       = on      # the last free pages are kept for sizes that own none
hardening     = on      # encoded free lists, double frees caught
purge         = on      # empty pages give their memory back after a burst
fullest_first = off     # measured: no gain on queue traffic
warm_pages    = auto    # empty pages kept ready before purging (auto = 1/8)
reserve_pages = auto    # pages the reserve holds back (auto = 1/64)
```

An unknown key or a bad value stops Quicx with the file and line, for example `quicx.conf:14: purge must be on or off, not 'sometimes'`. `--port` on the command line wins over the file. A 1.x config still loads: its `class` lines are ignored with a note.

## Wire protocol

Every message is a 6-byte header followed by a body; the payload inside a body is at most 1,024 bytes.

| Field | Size | |
|---|---|---|
| version | 1 B | `1` |
| type | 1 B | one of the messages below |
| length | 4 B | length of the body |

| Message | Sent by | Meaning |
|---|---|---|
| `SUBMIT` 0x01 | producer | a new task: its type and payload |
| `OK` 0x02 | daemon | accepted, with the task id |
| `ERROR` 0x03 | daemon | refused: `0x01` queue full, `0x02` invalid message, `0x03` payload too big, `0x04` unknown type, `0x05` version mismatch |
| `READY` 0x04 | worker | I can take a task |
| `TASK` 0x07 | daemon | a task for that worker |
| `DONE` 0x05 / `FAILED` 0x06 | worker | the result, passed on to the producer |
| `HEARTBEAT` 0x09 / `PONG` 0x0A | client / daemon | keep-alive |
| `STATS` 0x0B / 0x0C | CLI | live statistics, over a unix socket |

## Limits

Quicx keeps its promise about memory by doing less than a durable broker. It does not have:

- **Persistence.** A restart loses everything queued.
- **Redelivery.** A task is freed when it is sent, so if a worker dies mid-task the task is lost (at-most-once).
- **Retries, priorities, delays or named queues.** There is one global FIFO.
- **Payloads above 1,024 B.** Larger frames get `ERROR 0x03`.
- **More than one core.** Run several daemons behind a TCP load balancer.

Part of its speed is work it does not do. If losing a task on a worker crash is not acceptable, use a broker with at-least-once delivery such as beanstalkd or Redis Streams.

## Building and testing

You need a C17 compiler and `make`; there are no other dependencies.

```sh
make            # quicxd and the unit tests
make test       # test_apool (50 checks), test_config (40), test_wire (28)
make e2e        # 27 end-to-end checks against ./quicxd (needs python3)
make bench      # bench_apool and bench_purge, the allocator benchmarks
```

To reproduce the report and every number in this README, see [`docs/report`](docs/report/README.md).

## Repository

```
quicx/include, quicx/src   the daemon: event loop, protocol, server, daemon, queue, dispatcher,
                           apool (the allocator) and pool_config (its settings)
quicx/src/tests            unit tests and the allocator benchmarks
pmad/                      PMAD, the static allocator of v1.x, kept as the benchmark baseline
docs/report/               the apool v2 report: LaTeX, raw results, scripts, diagrams
docs/assets/               the images in this README (make_assets.py)
quicx.conf                 every setting with its default
```

## Compared with v1.0.4

<picture>
  <source media="(prefers-color-scheme: dark)" srcset="docs/assets/results-dark.png">
  <img src="docs/assets/results-light.png" alt="v2.0.0 fits 46,593 tasks of 100 B in 8 MiB where v1.0.4 fits 655, and after a burst holds 2,080 KB instead of 9,200 KB on macOS and 2,736 KB instead of 9,928 KB on Linux." width="100%">
</picture>

Up to 1.0.4, Quicx used **PMAD**, a static allocator that split the budget between block sizes by fixed percentages before the first task arrived. Most of the budget sat unused whenever the traffic did not match the split. Both versions were measured with the same scripts on the same two machines.

**Capacity: tasks accepted in 8 MiB** (the same on both machines)

| Payload | v1.0.4 | v2.0.0 | |
|---|---:|---:|---:|
| 100 B | 655 | **46,593** | 71× |
| 900 B | 180 | **8,016** | 45× |
| 40 B | 1,310 | **73,146** | 56× |
| 300 B | 262 | **22,044** | 84× |
| nginx log lines | 1,433 | **30,947** | 22× |

**Memory: physical memory of the daemon, KB**

| | v1.0.4 Mac | v2.0.0 Mac | v1.0.4 Linux | v2.0.0 Linux |
|---|---:|---:|---:|---:|
| idle, just started | 9,200 | **1,056** | 9,928 | **1,796** |
| queue full | 9,200 | 9,088 | 9,928 | 9,816 |
| after the burst drains | 9,200 | **2,080** | 9,928 | **2,736** |

v1.0.4 wrote a header into every block at start-up, so it held the whole pool from the first second.

**Speed** (median of 5 interleaved runs)

| | v1.0.4 Mac | v2.0.0 Mac | v1.0.4 Linux | v2.0.0 Linux |
|---|---:|---:|---:|---:|
| throughput, tasks/s | 82.4k | 81.9k | 19.0k | 19.0k |
| latency p50, µs | 47 | 49 | 128 | 123 |
| latency p99, µs | 111 | 98 | 162 | 198 |

The differences are inside the run-to-run ranges: the new allocator did not make the daemon faster or slower. On its own, PMAD's fixed lists are still the quickest per allocation (13.3 ns per task on the M4 Pro, against 14.8 ns for apool v2), and a round trip through the daemon takes tens of microseconds.

**Correctness: end-to-end checks.** v1.0.4 passes 23 of 27 and v2.0.0 passes **27 of 27**, on both machines. The four v1.0.4 failures are a deadlock: in PMAD, 16 B and 32 B tasks shared a size class with connections and workers. When the queue was full of such tasks, every new worker was locked out until a restart. The control pool fixes it.

## Papers and references

The ideas in apool come from these works. Each entry was checked against its publisher or two independent records; the [report](docs/report/main.pdf) cites them in full.

1. <a id="ref-1"></a>Jeff Bonwick. **The Slab Allocator: An Object-Caching Kernel Memory Allocator.** USENIX Summer 1994 Technical Conference, Boston. [usenix.org](https://www.usenix.org/legacy/publications/library/proceedings/bos94/bonwick.html) — *pages carved into blocks of one size.*
2. <a id="ref-2"></a>Emery D. Berger, Kathryn S. McKinley, Robert D. Blumofe, Paul R. Wilson. **Hoard: A Scalable Memory Allocator for Multithreaded Applications.** ASPLOS-IX, 2000, pp. 117–128. [doi:10.1145/378993.379232](https://doi.org/10.1145/378993.379232) — *fullness groups, fullest page first (measured, off).*
3. <a id="ref-3"></a>Daan Leijen, Benjamin Zorn, Leonardo de Moura. **Mimalloc: Free List Sharding in Action.** APLAS 2019, LNCS 11893, Springer. [doi:10.1007/978-3-030-34175-6_13](https://doi.org/10.1007/978-3-030-34175-6_13) — *free lists per page, encoded links (secure mode), giving memory back.*
4. <a id="ref-4"></a>Jason Evans. **A Scalable Concurrent malloc(3) Implementation for FreeBSD.** BSDCan 2006. [PDF](https://people.freebsd.org/~jasone/jemalloc/bsdcan2006/jemalloc.pdf) — *jemalloc; purging unused pages.*
5. <a id="ref-5"></a>Martin Maas, David G. Andersen, Michael Isard, Mohammad Mahdi Javanmard, Kathryn S. McKinley, Colin Raffel. **Learning-based Memory Allocation for C++ Server Workloads.** ASPLOS '20, pp. 541–556. [doi:10.1145/3373376.3378525](https://doi.org/10.1145/3373376.3378525) — *LLAMA; grouping objects by lifetime.*
6. <a id="ref-6"></a>Mel Gorman. **Understanding the Linux Virtual Memory Manager.** Prentice Hall, 2004, chapter 2 — *zone watermarks: keep the last free pages in reserve.*
7. <a id="ref-7"></a>Jonathan Corbet, Alessandro Rubini, Greg Kroah-Hartman. **Linux Device Drivers**, 3rd edition. O'Reilly, 2005, chapter 8 — *memory pools reserved for forward progress.*
8. <a id="ref-8"></a>memcached project. **Release Notes for v1.4.11: slab_reassign and slab_automove.** 2012. [github.com/memcached](https://github.com/memcached/memcached/wiki/ReleaseNotes1411) — *moving slab pages between size classes; a minimum per class.*
9. <a id="ref-9"></a>Linux man-pages project. **madvise(2).** [man7.org](https://man7.org/linux/man-pages/man2/madvise.2.html) — *`MADV_DONTNEED`.*
10. <a id="ref-10"></a>Apple. **XNU kernel source, `bsd/sys/mman.h`.** [github.com/apple-oss-distributions](https://github.com/apple-oss-distributions/xnu/blob/main/bsd/sys/mman.h) — *`MADV_FREE_REUSABLE`.*
11. <a id="ref-11"></a>Simon Brown. **The C4 model: notation and diagram review checklist.** [c4model.com](https://c4model.com/diagrams/checklist) — *the rules the architecture diagrams follow.*

## Licence

MIT — see [LICENSE](LICENSE). Diagram icons are from [Lucide](https://lucide.dev) (ISC); fonts are Archivo and JetBrains Mono (SIL Open Font License).
