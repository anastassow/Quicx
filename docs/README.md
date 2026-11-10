# Where every number comes from

Each claim in the README, the website and the report comes from a script in this repository and a file of raw output next to it. Run the script to get the number again.

| Claim | Script | Raw results |
|---|---|---|
| tasks that fit in a budget (46,593 of 100 B in 8 MiB, and every other size) | `docs/report/scripts/capacity.py`, run by `docs/report/scripts/run_all.sh` | `docs/report/results/mac`, `docs/report/results/server` |
| the capacity formula matches the daemon | the formula in the README against `capacity.py` | `docs/report/results/mac/capacity_*` |
| throughput and latency (81,900 tasks/s, 49 µs p50 on the M4 Pro) | `docs/report/scripts/bench/run.sh` (C producers and workers) via `run_all.sh` | `docs/report/results/mac/closed_*`, `open_*`, and the same under `server/` |
| physical memory before, during and after a burst | `capacity.py` via `run_all.sh` (it samples the process while it fills and drains) | `docs/report/results/mac/capacity_*`, `server/capacity_*` |
| allocator cost per task, each technique on and off | `bench_apool`, `bench_purge` (`make bench`) | `docs/report/results/*/bench_*` |
| valuable tasks kept when the budget is full (100 % against 31.6 %) | `docs/report/scripts/admission/bench_admission.py` | `docs/report/results/admission` |
| several daemons: 1.92x on two cores, an even split on one machine | `docs/report/scripts/several/run_several.sh`, `run_remote.sh` | `docs/report/results/several` |
| Quicx and Redis in 64 MB containers, and at the same limit | `docs/report/scripts/redis/run.sh` | `docs/report/results/redis` |
| learned task values (66.7 % against 27.7 % of diseased frames) | `docs/research/learned-value/learned_value_sim.py` | `docs/research/learned-value/results` |
| bigger payloads: 1,002 tasks of 8,000 B in 8 MiB, as the formula says | `make check-big` (`docs/report/scripts/big_payload_check.py`) | the output of the run |
| unit checks (155) | `make test` | the output of the run |
| end-to-end checks (27) | `make e2e` (`docs/report/scripts/e2e_tests.py`) | `docs/report/results/*/e2e_*` |

The machines: an Apple M4 Pro (12 cores, macOS 26.5) and an Intel Core i3-2100 from 2011 (2 cores, 4 threads, Ubuntu 24.04). [`report/README.md`](report/README.md) explains how to rebuild the whole report, and [`report/FINDINGS.md`](report/FINDINGS.md) lists what the runs found.
