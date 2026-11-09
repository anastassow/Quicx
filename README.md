<p align="center">
  <picture>
    <source media="(prefers-color-scheme: dark)" srcset="docs/assets/banner-dark.png">
    <img src="docs/assets/banner-light.png" alt="Quicx — the task queue with a hard memory limit" width="100%">
  </picture>
</p>

<p align="center">
  <a href="https://quicx.dev"><img alt="quicx.dev" src="https://img.shields.io/badge/quicx.dev-website-ff5700?style=flat-square&labelColor=0a171d"></a>
  <img alt="version 2.0.0" src="https://img.shields.io/badge/version-2.0.0-ff5700?style=flat-square&labelColor=0a171d">
  <img alt="C17" src="https://img.shields.io/badge/C17-no%20dependencies-4b5f68?style=flat-square&labelColor=0a171d">
  <img alt="macOS and Linux" src="https://img.shields.io/badge/macOS%20%7C%20Linux-kqueue%20%7C%20epoll-4b5f68?style=flat-square&labelColor=0a171d">
  <a href="LICENSE"><img alt="MIT licence" src="https://img.shields.io/badge/licence-MIT-4b5f68?style=flat-square&labelColor=0a171d"></a>
</p>

**Quicx is a task queue daemon written in C that never uses more memory than you give it.** Producers submit tasks over TCP, workers take them one at a time, and the producer hears back when each one is done. The whole daemon is one process and one thread. Its memory is one budget fixed at start-up (8 MiB by default): when the budget is full, Quicx refuses new tasks with a clean error instead of growing.

Version 2 replaces the allocator inside that budget. The old one split the memory between block sizes by fixed percentages, so most of it sat unused whenever the traffic did not match the split. The new one, **apool v2**, hands 16 KiB pages to whatever sizes are actually arriving and takes them back when they empty. The same 8 MiB now holds **46,593** tasks of 100 B instead of **655**, and gives its memory back to the operating system after a burst.

[Website](https://quicx.dev) · [Documentation](https://quicx.dev/docs) · [Report (PDF)](docs/report/main.pdf) · [Findings](docs/report/FINDINGS.md)

---

## Contents

- [How it works](#how-it-works)
- [What is new in 2.0](#what-is-new-in-20)
- [Benchmarks](#benchmarks)
- [Install](#install)
- [Quick start](#quick-start)
- [Configuration](#configuration)
- [Wire protocol](#wire-protocol)
- [Limits](#limits)
- [Building and testing](#building-and-testing)
- [Repository](#repository)

## How it works

<picture>
  <source media="(prefers-color-scheme: dark)" srcset="docs/report/figures/quicx-architecture-dark.png">
  <img src="docs/report/figures/quicx-architecture-light.png" alt="Quicx component diagram: producers and workers talk to quicxd over TCP; inside, an event loop and the protocol feed the server, daemon, queue and dispatcher; every task lives in one block of the task pool from SUBMIT until it is sent as TASK." width="100%">
</picture>

One task, step by step (the orange path above):

1. A producer sends **SUBMIT** with a type and a payload of up to 1,024 B. The event loop (kqueue on macOS, epoll on Linux) wakes up for it.
2. The server takes **one block** from the task pool for the task, a 64 B record with the payload right behind it, and reads the payload straight into place. If there is no room, the producer gets `ERROR 0x01` and the connection stays in sync.
3. The daemon **queues** the task (one FIFO) and answers the producer with **OK** and the task id.
4. The dispatcher **pops** the oldest task as soon as a worker has said **READY**.
5. The task is **sent** to that worker as **TASK**.
6. Its block is **freed** at once, so delivery is at-most-once. The worker's **DONE** or **FAILED** is passed on to the producer.

Connections and workers live in a separate control pool, so a queue full of tasks can never stop a worker from registering and draining it.

## What is new in 2.0

<picture>
  <source media="(prefers-color-scheme: dark)" srcset="docs/report/figures/apool-architecture-dark.png">
  <img src="docs/report/figures/apool-architecture-light.png" alt="apool v2: one mapping of 16 KiB pages and 32 B page records; size classes own lists of pages; freed blocks inside a page form an encoded free list; empty pages stay warm, then give their memory back." width="100%">
</picture>

Every idea in apool v2 comes from a published allocator and sits behind its own switch, so its effect was measured on its own. These are the ones Quicx 2.0 ships with, and what each one bought in the measurements:

| Feature | Idea from | What it buys | What it costs |
|---|---|---|---|
| **Pages move between sizes** | slab allocator (Bonwick 1994), mimalloc pages, memcached | 46,593 tasks of 100 B fit instead of 655; the pool follows size changes without a restart | — |
| **One block per task** | Quicx | one allocation and one free per task: 10 ns less per task on the M4 Pro, 48 ns on the i3 | — |
| **Control pool** | Quicx | fixes a v1.0.4 deadlock: a queue full of 16 B or 32 B tasks locked out every new worker | 3 of the budget's pages |
| **Reserve** | Linux zone watermarks, memcached | after a flood of one size, a new size still gets room (16 tasks of 900 B instead of 0) | 1.4 % of capacity at 100 B |
| **Hardening** | mimalloc secure mode | double frees and overwritten free-list links are caught instead of corrupting memory | 1 ns per task (M4 Pro), 13 ns (i3) |
| **Purge with a warm list** | jemalloc, mimalloc | memory goes back after a burst: 1,104 KB held instead of 8,096 KB (allocator benchmark) | only on bursts that fill and empty the whole pool: 102 instead of 8.7 ns per block (M4 Pro) |

Each one can be switched in [`quicx.conf`](#configuration). Two more ideas were measured and are not used by the daemon: fullest-page-first (Hoard) gave no gain on queue traffic, and lifetime classes (LLAMA) only help when a caller marks long-lived tasks, which the daemon does not do yet. The [report](docs/report/main.pdf) has every source, the place in the code, and all the numbers.

## Benchmarks

<picture>
  <source media="(prefers-color-scheme: dark)" srcset="docs/assets/results-dark.png">
  <img src="docs/assets/results-light.png" alt="v2.0.0 fits 46,593 tasks of 100 B in 8 MiB where v1.0.4 fits 655, and after a burst holds 2,080 KB instead of 9,200 KB on macOS and 2,736 KB instead of 9,928 KB on Linux." width="100%">
</picture>

Quicx 2.0 (with the features above) against 1.0.4, both built with `-O2`, on two very different machines:

| | Mac | Linux PC |
|---|---|---|
| CPU | Apple M4 Pro, 12 cores | Intel Core i3-2100 (2011), 2 cores / 4 threads |
| OS, compiler | macOS 26.5, Apple clang 17 | Ubuntu 24.04, gcc 13.3 |

**Capacity: tasks the daemon accepts before it says "full"** (the same on both machines)

| Payload | v1.0.4 | v2.0.0 |
|---|---:|---:|
| 100 B | 655 | **46,593** |
| 900 B | 180 | **8,016** |
| 40 B | 1,310 | **73,146** |
| 300 B | 262 | **22,044** |
| nginx log lines (avg. 188 B) | 1,433 | **30,947** |

The sizes are sent one after another to the same daemon, and the queue is drained in between. v2 follows each change; v1.0.4 is stuck with the split it started with.

**Memory: physical memory of the daemon, KB** (median of 3 runs)

| | v1.0.4 Mac | v2.0.0 Mac | v1.0.4 Linux | v2.0.0 Linux |
|---|---:|---:|---:|---:|
| idle, just started | 9,200 | **1,056** | 9,928 | **1,796** |
| queue full | 9,200 | 9,088 | 9,928 | 9,816 |
| after the burst drains | 9,200 | **2,080** | 9,928 | **2,736** |

macOS measures `phys_footprint`, Linux resident set size, so compare within a column pair.

**Speed: the allocator does not slow the daemon down** (median of 5 interleaved runs)

| | v1.0.4 Mac | v2.0.0 Mac | v1.0.4 Linux | v2.0.0 Linux |
|---|---:|---:|---:|---:|
| throughput, tasks/s | 82.4k | 81.9k | 19.0k | 19.0k |
| latency p50, µs | 47 | 49 | 128 | 123 |
| latency p99, µs | 111 | 98 | 162 | 198 |

Throughput: 8 workers, 2 producers with 4 connections each and 32 tasks in flight per connection, for 10 s. Latency: submit to done, at a fixed 40,000 tasks/s on the Mac and 9,000 on the Linux PC. The differences between the versions are inside the run-to-run ranges. On the Linux PC eleven processes share 4 hardware threads, so its CPU is the limit.

**Correctness: end-to-end checks** — v1.0.4 passes 23 of 27 and v2.0.0 passes **27 of 27**, on both machines. The four v1.0.4 failures are the deadlock the control pool fixes.

Every number comes from the raw output in [`docs/report/results`](docs/report/results), produced by `docs/report/scripts/run_all.sh`. The [report](docs/report/main.pdf) explains each benchmark and its limits.

## Install

```sh
curl -fsSL https://quicx.dev/install.sh | sh
```

The installer puts a single `quicx` binary for macOS or Linux (x86-64 or arm64) into `/usr/local/bin`. To build it yourself, see [Building and testing](#building-and-testing).

## Quick start

```sh
quicx start                          # 8 MiB budget, port 16381
quicx start --config quicx.conf      # or with a config file
quicx status                         # the pool, the queue and the workers, live
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

Producers and workers can use the Java client (`dev.quicx:quicx-client`, see the [quick start](https://quicx.dev/docs/quick-start)) or speak the small [wire protocol](#wire-protocol) directly from any language.

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

An unknown key or a bad value stops Quicx with the file and line, for example `quicx.conf:14: purge must be on or off, not 'sometimes'`. `--port` on the command line wins over the file.

## Wire protocol

Every message is a 6-byte header followed by a body; the payload inside a body is at most 1,024 bytes:

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
| `STATS` 0x0B / 0x0C | CLI | live statistics over a unix socket |

The full format of every body is in the [protocol documentation](https://quicx.dev/docs/binary-protocol).

## Limits

Quicx keeps its promise about memory by doing less than a durable broker. It does not have:

- **Persistence.** A restart loses everything queued.
- **Redelivery.** A task is freed when it is sent, so if a worker dies mid-task the task is lost (at-most-once).
- **Retries, priorities, delays or named queues.** One global FIFO.
- **Payloads above 1,024 B.** Larger frames get `ERROR 0x03`.
- **More than one core.** Run several daemons behind a TCP load balancer.

If losing a task on a worker crash is not acceptable, use a broker with at-least-once delivery such as beanstalkd or Redis Streams.

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

## Licence

MIT — see [LICENSE](LICENSE). Diagram icons are from [Lucide](https://lucide.dev) (ISC); fonts are Archivo and JetBrains Mono (SIL Open Font License).
