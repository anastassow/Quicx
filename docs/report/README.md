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
