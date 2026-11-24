# apool v2 report

The LaTeX report on Quicx's adaptive allocator: every feature taken from a
paper, where it is in the code, the tests and benchmarks that cover it, and
their results on a Mac (Apple M4 Pro) and a Linux PC (Intel Core i3-2100).

**Read it:** [`main.pdf`](main.pdf) (13 pages)

| Folder | What is in it |
|---|---|
| `main.tex`, `refs.bib` | the report and its references (each one checked against the publisher or two records) |
| `figures/` | the architecture diagrams, dark and light, as SVG, PDF and PNG |
| `results/mac`, `results/server` | the raw output of every run; every number in the report comes from these |
| `tables/`, `values.tex` | generated from `results/` by `scripts/make_tables.py` |
| `scripts/` | build, run, tables, diagrams (see below) |
| `FINDINGS.md` | what the runs found, each with a way to reproduce it |
| `fonts/`, `icons/` | Archivo and JetBrains Mono (OFL), Lucide icons (ISC), for the diagrams |

## Reproduce

```sh
cd docs/report
scripts/build.sh mac            # binaries into build/mac/ (also extracts v1.0.4 and the first adaptive pool from git)
scripts/run_all.sh mac          # every test and benchmark, raw output into results/mac/ (about 15 minutes)
scripts/run_server.sh user@host # the same on a Linux machine over ssh, results copied back
python3 scripts/make_tables.py  # tables/ from results/
scripts/export_figures.sh       # the diagrams (needs fontTools and rsvg-convert)
tectonic -X compile main.tex    # main.pdf
```

## File paths in the report

The report describes Quicx 2.0, when the daemon was in `quicx/src` and
`quicx/include` and the unit tests in `quicx/src/tests`. Since then the code is
in one folder per part, and the report's line numbers belong to the commit it
was built from. To read the code exactly as the report describes it, open
[`bb1ce58`](https://github.com/anastassow/Quicx/tree/bb1ce58). Where each file is now:

| In the report | Now |
|---|---|
| `quicx/src/apool.c` | `src/alloc/apool.c` |
| `quicx/src/protocol.c` | `src/qxp/protocol.c` |
| `quicx/src/event_loop.c`, `server.c` | `src/net/` |
| `quicx/src/daemon.c`, `queue.c`, `dispacher.c`, `workers.c`, `stats.c`, `quicx.c` | `src/core/` (`dispacher.c` is spelled `dispatcher.c`) |
| `quicx/src/config.c`, `pool_config.c` | `src/config/` |
| `quicx/src/cli.c`, `quicx/include/cli.h` | `src/cli/` |
| `quicx/include/*.h` | next to its `.c` file |
| `quicx/src/tests/test_*.c` | `tests/unit/` |
| `quicx/src/tests/bench_*.c`, `pmad_layout.c` | `bench/` |
| `pmad/` | `bench/pmad/` |
| `scripts/e2e_tests.py`, `scripts/qwire.py`, `scripts/big_payload_check.py` | `tests/e2e/` |
