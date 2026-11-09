# Findings — Quicx apool v2 report (8 Nov 2026, Mac and Linux server runs)

Each finding has a repro. Nothing was fixed in the code; these are for you to decide.

## 1. Released v1.0.4 can deadlock with 16 B or 32 B tasks (bug)

With PMAD, a 16 B payload shares a size class with `Connection` (16 B) and a 32 B payload with `Worker` (32 B). If the queue fills with such tasks while no worker is connected, a worker that connects afterwards can never register:

- 16 B: the daemon closes the new connection at once (`out of memory for connection`); drained 0/5,240
- 32 B: the connection is accepted but READY gets `ERROR worker pool full`; drained 0/3,494

The queue then stays full until the daemon restarts. v1 and v2 pass (separate control pool). The Linux server (`da`) fails the same two tests with the same messages, so it is not macOS-specific.

Repro:
```
cd docs/report
scripts/build.sh mac
python3 scripts/e2e_tests.py build/mac/quicxd-1.0.4 16391    # liveness-16 and liveness-32 FAIL
python3 scripts/e2e_tests.py build/mac/quicxd-v2 16391       # all 27 PASS
```
Raw output: `results/mac/e2e_1.0.4.txt`, `results/server/e2e_1.0.4.txt`. Fix for a 1.0.5: give Connections and Workers their own pool (as v2 does), or keep a few PMAD blocks of 16 B and 32 B for them.

## 2. Purge makes pool-sized bursts slower

`bench_purge`: filling and emptying the *whole* pool 40 times costs 102.1 ns per block with purge on, against 8.7 ns with it off. Every cycle purges and reuses 441 pages (504 used, 63 stay warm). Steady churn never purges (0) and costs the same either way (12.6 vs 12.4 ns). Memory after a drain: 1,104 KB vs 8,096 KB above baseline. Server (i3-2100, Linux): 584.7 vs 51.1 ns per block (the same ~11× ratio), 1,280 vs 8,080 KB after a drain; on Linux `MADV_DONTNEED` drops the pages, so every reuse faults them in and zeroes them.

Repro: `build/mac/bench_purge`. Raw: `results/mac/bench_purge_*.txt`. Possible change: purge only after a delay (as jemalloc's decay does), or raise `warm`.

## 3. Fullest-first (Hoard) does not help queue traffic

Deepest FIFO mixed queue 29,952 → 29,696 with it, and 14.8 → 20.0 ns per task. It stays off. Raw: `results/mac/bench_apool_*.txt`.

## 4. Lifetime classes are not used by the daemon

They lift the pinned case from 16 to 7,648 tasks of 900 B, but only when a caller passes a hint. Today the daemon frees tasks in arrival order and passes none.

## 5. The reserve costs a little capacity

100 B capacity 47,523 → 46,872 (1.4%); deepest FIFO mixed queue 30,208 → 29,952. That is the price of the starve case going from 0 to 16 tasks.

## 6. 16 KiB pages are the best of 4–64 KiB

4 KiB: 22.2 ns per task and only 4 tasks for a newcomer; 64 KiB: deepest mixed queue drops to 23,680. 16 KiB: 14.9 ns, 29,952. Raw: `results/mac/bench_ps*_*.txt`.

## 7. Open-loop tail latency varies a lot between runs, on both machines

p99.9 ranged from 129 to 4,470 µs on the Mac and from 137 to 4,296 µs on the server between runs of the *same* version; the medians are close (Mac: 1.0.4 252, v1 163, v2 143 µs; server: 207, 214, 208 µs). No version is consistently worse. Raw: `results/*/open_*.txt`.

## 8. PMAD touches all its memory at start-up

1.0.4 has a 9,200 KB physical footprint from the moment it starts (server: 9,928 KB resident). v2 starts at 1,056 KB and returns to 2,080 KB after each burst (server: 1,796 → 9,816 → 2,736 KB). Raw: `results/mac/capacity_*.txt`.

## 9. The adaptive-pool branch said v1.0.3 (fixed)

During the runs `quicxd-v2 version` printed `quicx v1.0.3`, because the branch was made before the 1.0.4 bump. `VERSION` in `quicx/include/cli.h` is 2.0.0 now.

## 10. Hardening costs much more on an older CPU (new, from the server)

`bench_apool`, ns per task: M4 Pro 13.8 → 14.8 with hardening (about 1 ns); i3-2100 60.0 → 72.8 (about 13 ns, 21%). PMAD's fixed lists are the fastest per allocation on both machines (13.3 and 43.3 ns), so apool pays for 71× the capacity with extra nanoseconds. The daemon's throughput does not change either way (19.0k tasks/s for all three versions on the server, limited by 4 hardware threads for 11 processes). Raw: `results/server/bench_apool_*.txt`.

## 11. Capacity does not depend on the machine

Every "tasks that fit" number is identical on the Mac (16 KiB OS pages) and the server (4 KiB OS pages), in `bench_apool`, the page-size sweep and the whole daemon. Raw: `results/*/capacity_*.txt`.

## 12. The features work together (checked 9 Nov)

The old stress test said "all features" but ran without fullest-first (it never set the flag). Fixed: it now runs twice, with the daemon's flags and with `APOOL_F_ALL`, and also checks that pages were reshaped and purged during the run (50/50 checks on both machines). A daemon with every feature on (`fullest_first = on` in quicx.conf; the test build `build/*/quicxd-v2-all` sets that default with `-DQUICX_POOL_FLAGS=APOOL_F_ALL` in `pool_config.c`) passes all 27 end-to-end checks with the same capacity as v2 and still returns its memory (Mac 2,096 KB, server 2,740 KB after the last burst). In the shipped daemon 6 of 8 features are active; fullest-first is off by choice, lifetime hints are never passed. Raw: `results/*/test_apool.txt`, `e2e_v2-all.txt`, `capacity_v2-all_*.txt`.

