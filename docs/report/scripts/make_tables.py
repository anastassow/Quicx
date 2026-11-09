#!/usr/bin/env python3
"""
make_tables.py — turns results/<machine>/ raw output into LaTeX tables and summary.json.

    make_tables.py            (from docs/report; machines: mac, server)

Every number in the report's result tables comes from here, so it can be
traced back to a file in results/. Missing machines show as "—".
"""
import glob
import json
import os
import re
import statistics as stat

REPORT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")
RES = os.path.join(REPORT, "results")
TAB = os.path.join(REPORT, "tables")
MACHINES = ["mac", "server"]
VERSIONS = ["1.0.4", "v1", "v2"]
EXTRA = ["v2-all"]          # v2 with every allocator feature on (QUICX_POOL_FLAGS=APOOL_F_ALL)
os.makedirs(TAB, exist_ok=True)


def read(path):
    try:
        return open(path, encoding="utf-8", errors="replace").read()
    except OSError:
        return None


def kv(line):
    return dict(re.findall(r'(\w+)=("[^"]*"|\S+)', line))


def fmt(n):
    if n is None:
        return "—"
    if isinstance(n, float) and not n.is_integer():
        return f"{n:,.1f}"
    return f"{int(n):,}"


def spread(xs, digits=1, unit=""):
    """median with the min–max range, for repeated timings"""
    if not xs:
        return "—"
    m, lo, hi = stat.median(xs), min(xs), max(xs)
    f = (lambda v: f"{v:,.{digits}f}")
    if lo == hi:
        return f"{f(m)}{unit}"
    return f"{f(m)}{unit} {{\\scriptsize ({f(lo)}–{f(hi)})}}"


def same(xs):
    """value of a count that should not change between runs; marks it if it did"""
    if not xs:
        return "—"
    if len(set(xs)) == 1:
        return fmt(xs[0])
    return f"{fmt(stat.median(xs))}$^\\dagger$"


# ------------------------------------------------------------ parsers
BENCH_COLS = ["cap100", "ph100", "ph900", "ph40", "ph300", "starve", "pinned", "fifo", "out8", "random", "ns"]


def parse_bench(text):
    rows = {}
    for line in text.splitlines():
        m = re.match(r"^(.{22}) +(\d[\d .|]+)$", line)
        if not m:
            continue
        nums = re.findall(r"\d+(?:\.\d+)?", m.group(2))
        if len(nums) == len(BENCH_COLS):
            rows[m.group(1).strip()] = {k: float(v) for k, v in zip(BENCH_COLS, nums)}
    return rows


def load(machine):
    d = os.path.join(RES, machine)
    if not os.path.isfile(os.path.join(d, "machine.txt")):
        return None
    out = {"machine": read(os.path.join(d, "machine.txt")) or ""}

    # unit tests
    ta = read(os.path.join(d, "test_apool.txt")) or ""
    sections, cur = {}, None
    for line in ta.splitlines():
        m = re.match(r"^--- (.+?) ---", line)
        if m:
            cur = m.group(1)
            sections[cur] = [0, 0]
        elif cur and line.strip().startswith(("PASS", "FAIL")):
            sections[cur][0 if line.strip().startswith("PASS") else 1] += 1
    out["test_apool"] = sections
    m = re.search(r"passed: (\d+)\s+failed: (\d+)", ta)
    out["test_apool_total"] = (int(m.group(1)), int(m.group(2))) if m else None
    tw = read(os.path.join(d, "test_wire.txt")) or ""
    p, f = re.search(r"passed: (\d+)", tw), re.search(r"failed: (\d+)", tw)
    out["test_wire"] = (int(p.group(1)), int(f.group(1))) if p and f else None

    # e2e
    e2e = {}
    for v in VERSIONS + EXTRA:
        t = read(os.path.join(d, f"e2e_{v}.txt")) or ""
        e2e[v] = {m.group(1): (m.group(2) == "PASS", m.group(3).strip())
                  for m in re.finditer(r"^RESULT (\S+) (PASS|FAIL) ?(.*)$", t, re.M)}
    out["e2e"] = e2e

    # bench_apool (5 runs) and page-size sweep (3 runs)
    def bench_runs(pattern):
        runs = [parse_bench(read(f) or "") for f in sorted(glob.glob(os.path.join(d, pattern)))]
        merged = {}
        for r in runs:
            for name, vals in r.items():
                for k, v in vals.items():
                    merged.setdefault(name, {}).setdefault(k, []).append(v)
        return merged, len(runs)

    out["bench"], out["bench_runs"] = bench_runs("bench_apool_*.txt")
    out["ps"] = {ps: bench_runs(f"bench_ps{ps}_*.txt")[0] for ps in (4, 8, 16, 32, 64)}

    # bench_purge
    purge = {}
    for f in sorted(glob.glob(os.path.join(d, "bench_purge_*.txt"))):
        for line in (read(f) or "").splitlines():
            k = kv(line)
            if "mode" in k:
                for key, val in k.items():
                    if key not in ("mode", "measure"):
                        purge.setdefault(k["mode"], {}).setdefault(key, []).append(float(val))
                purge.setdefault(k["mode"], {})["measure"] = k.get("measure")
    out["purge"] = purge

    # capacity
    cap = {}
    for v in VERSIONS + EXTRA:
        for f in sorted(glob.glob(os.path.join(d, f"capacity_{v}_*.txt"))):
            for line in (read(f) or "").splitlines():
                k = kv(line)
                ph = k.get("phase")
                if not ph:
                    continue
                slot = cap.setdefault(v, {}).setdefault(ph, {})
                for key in ("accepted", "mem_full_kb", "mem_after_drain_kb", "mem_kb"):
                    if key in k:
                        slot.setdefault(key, []).append(float(k[key]))
                if "measure" in k:
                    cap[v]["measure"] = k["measure"]
                if "drained" in k:
                    a, b = k["drained"].split("/")
                    slot.setdefault("drained_ok", []).append(a == b)
    out["capacity"] = cap

    # throughput / latency
    def runs(kind, v):
        xs = []
        for f in sorted(glob.glob(os.path.join(d, f"{kind}_{v}_*.txt"))):
            lines = [l for l in (read(f) or "").splitlines() if "p50=" in l]
            if not lines:
                continue
            k = kv(lines[-1])
            thr = sum(float(t[:-1]) * 1000 if t.endswith("k") else float(t)
                      for t in re.search(r"thr=\[([^\]]*)\]", lines[-1]).group(1).split())
            xs.append({"p50": float(k["p50"]), "p99": float(k["p99"]), "p999": float(k["p999"]),
                       "thr": thr, "err": int(k["err"]), "drops": int(k["drops"])})
        return xs

    out["closed"] = {v: runs("closed", v) for v in VERSIONS}
    out["open"] = {v: runs("open", v) for v in VERSIONS}
    m = re.search(r"open_loop_rate: (\d+)", out["machine"])
    out["rate"] = int(m.group(1)) if m else None
    return out


data = {m: load(m) for m in MACHINES}
present = [m for m in MACHINES if data[m]]
NAME = {"mac": "Mac", "server": "Server"}


def w(name, body):
    with open(os.path.join(TAB, name), "w") as f:
        f.write(body)


# ------------------------------------------------------------ machines
def machine_rows():
    def g(t, key):
        mm = re.search(rf"^{re.escape(key)}:\s*(.+)$", t, re.M)
        return mm.group(1).strip() if mm else None

    def cores(t):
        if g(t, "cores"):
            return g(t, "cores")
        c, tpc, cps = g(t, "lscpu: CPU(s)"), g(t, "lscpu: Thread(s) per core"), g(t, "lscpu: Core(s) per socket")
        if c and tpc and cps:
            return f"{cps} cores, {c} threads"
        return c

    def ram(t):
        if g(t, "ram_bytes"):
            return f"{int(g(t, 'ram_bytes')) / 2**30:.0f} GiB"
        if g(t, "ram_kb"):
            return f"{int(g(t, 'ram_kb')) / 2**20:.1f} GiB"
        return None

    def kernel(t):
        if g(t, "kernel"):
            return "Linux " + g(t, "kernel")
        u = g(t, "uname")
        if u and u.startswith("Darwin"):
            return "Darwin " + u.split()[2]
        return None

    num = lambda key, unit: (lambda t: f"{int(g(t, key)):,} {unit}" if g(t, key) else None)
    spec = [("CPU", lambda t: g(t, "cpu")), ("Cores", cores), ("RAM", ram), ("OS", lambda t: g(t, "os")),
            ("Kernel", kernel), ("OS page size", num("os_page_size", "B")),
            ("CPU governor", lambda t: g(t, "cpu_governor")),
            ("Compiler", lambda t: g(t, "compiler")), ("Open-loop rate", num("open_loop_rate", "tasks/s"))]
    rows = []
    for label, fn in spec:
        cells = []
        for m in MACHINES:
            v = (fn(data[m]["machine"]) if data[m] else None) or "—"
            cells.append(v.replace("_", r"\_").replace("&", r"\&").replace("%", r"\%").replace("#", r"\#"))
        if any(c != "—" for c in cells):
            rows.append((label, cells))
    seen, body = set(), ""
    for label, cells in rows:
        if label in seen and all(c == "—" for c in cells):
            continue
        seen.add(label)
        body += f"{label} & " + " & ".join(cells) + r" \\" + "\n"
    w("machines.tex", body)


# ------------------------------------------------------------ unit tests
APOOL_SECTIONS = [
    ("random stress, daemon features", "random sizes and order, 1 in 8 blocks long-lived, with the daemon's flags: no overlap, all memory returns, pages reshaped and purged", "core, reserve, hardening, purge, lifetimes together"),
    ("random stress, all features", "the same with every allocator feature on at once", "all six allocator features together"),
    ("reshape: all 64 B, drain, all 1024 B", "the same pages serve 64 B, then 1,024 B blocks", "pages move between sizes"),
    ("reserve", "a flood leaves the reserve; a new size still gets one page", "reserve"),
    ("fullest page first", "allocation picks the fuller page", "fullest-first"),
    ("lifetime classes", "short- and long-lived blocks never share a page", "lifetime classes"),
    ("double free", "a second free is refused and counted", "hardening"),
    ("corrupted free list", "an overwritten link is detected and cut", "hardening"),
    ("purge", "pages beyond the warm set are purged and work again", "purge"),
    ("invalid input", "bad sizes and bad pointers are refused", "core"),
]


def unit_tests():
    body = ""
    for sec, what, feat in APOOL_SECTIONS:
        cells = []
        for m in MACHINES:
            r = data[m]["test_apool"].get(sec) if data[m] else None
            cells.append("—" if not r else (f"{r[0]}/{r[0] + r[1]} pass" if r[1] == 0 else f"\\textbf{{{r[1]} fail}}"))
        body += f"\\texttt{{test\\_apool}}: {sec.split(':')[0]} & {what} & {feat} & " + " & ".join(cells) + r" \\" + "\n"
    cells = []
    for m in MACHINES:
        r = data[m]["test_wire"] if data[m] else None
        cells.append("—" if not r else (f"{r[0]}/{r[0] + r[1]} pass" if r[1] == 0 else f"\\textbf{{{r[1]} fail}}"))
    body += r"\texttt{test\_wire} & frames with payloads reach the socket intact & protocol fix (v1.0.4) & " + " & ".join(cells) + r" \\" + "\n"
    w("unit_tests.tex", body)


E2E_GROUPS = [
    ("roundtrip", "payloads of 0–1,019 B arrive byte-exact; DONE reaches the producer", "one block per task"),
    ("oversize", "a body over 1,024 B gets ERROR 0x03 and the connection closes", "protocol"),
    ("refusal-100", "a full queue answers ERROR 0x01; the stream stays in sync", "drain on refusal"),
    ("liveness-16", "after the queue fills with 16 B tasks, a new worker connects and drains all", "control pool"),
    ("liveness-32", "the same with 32 B tasks", "control pool"),
    ("liveness-100", "the same with 100 B tasks", "control pool"),
    ("liveness-900", "the same with 900 B tasks", "control pool"),
    ("stale", "DONE from a connection that never sent READY does not crash", "Connection reset"),
]


def e2e():
    body = ""
    for grp, what, feat in E2E_GROUPS:
        cells = []
        for m in MACHINES:
            for v in ("1.0.4", "v2"):
                if not data[m]:
                    cells.append("—")
                    continue
                checks = {k: r for k, r in data[m]["e2e"].get(v, {}).items() if k.split("/")[0] == grp}
                if not checks:
                    cells.append("—")
                    continue
                bad = [k for k, (ok, _) in checks.items() if not ok]
                cells.append("pass" if not bad else r"\textbf{fail}")
        body += f"{grp} & {what} & {feat} & " + " & ".join(cells) + r" \\" + "\n"
    w("e2e_tests.tex", body)
    # failure details
    det = ""
    for m in present:
        for v in VERSIONS:
            for k, (ok, why) in data[m]["e2e"].get(v, {}).items():
                if not ok:
                    det += f"\\item {NAME[m]}, {v}, \\texttt{{{k.replace('_', chr(92) + '_')}}}: {why.replace('_', chr(92) + '_')}\n"
    w("e2e_failures.tex", det or "\\item none\n")


# ------------------------------------------------------------ ablation
ABL = ["PMAD, default config", "apool v1", "apool v2", "v2 + fullest first", "v2 - reserve",
       "v2 - hardening", "v2 - lifetime hints", "v2 - one block/task"]


def ablation():
    for m in present:
        b = data[m]["bench"]
        body = ""
        for name in ABL:
            r = b.get(name)
            if not r:
                continue
            body += (f"{name.replace(' - ', ' $-$ ').replace(' + ', ' $+$ ')} & {same(r['cap100'])} & "
                     f"{same(r['ph900'])} & {same(r['ph40'])} & {same(r['ph300'])} & {same(r['starve'])} & "
                     f"{same(r['pinned'])} & {same(r['fifo'])} & {same(r['random'])} & {spread(r['ns'])}" + r" \\" + "\n")
        w(f"ablation_{m}.tex", body)
        w(f"ablation_{m}_runs.tex", str(data[m]["bench_runs"]))


def pagesize():
    body = ""
    for ps in (4, 8, 16, 32, 64):
        cells = []
        for m in MACHINES:
            r = data[m]["ps"][ps].get("apool v2") if data[m] else None
            if not r:
                cells += ["—"] * 4
                continue
            cells += [same(r["cap100"]), same(r["starve"]), same(r["fifo"]), spread(r["ns"])]
        body += f"{ps} KiB & " + " & ".join(cells) + r" \\" + "\n"
    w("pagesize.tex", body)


def purge():
    body = ""
    for m in MACHINES:
        for mode, label in (("purge_on", "purge on (default)"), ("purge_off", "purge off")):
            r = data[m]["purge"].get(mode) if data[m] else None
            if not r:
                cells = ["—"] * 5
            else:
                cells = [spread(r["full_kb"], 0), spread(r["held_after_drain_kb"], 0), spread(r["cycle_ns_per_block"]),
                         spread(r["churn_ns_per_task"]), same(r["churn_purges"])]
            body += f"{NAME[m]} & {label} & " + " & ".join(cells) + r" \\" + "\n"
    w("purge.tex", body)
    meas = {m: (data[m]["purge"].get("purge_on", {}).get("measure") if data[m] else None) for m in MACHINES}
    w("purge_measure.tex", ", ".join(f"{NAME[m]}: \\texttt{{{(meas[m] or '—').replace('_', chr(92) + '_')}}}"
                                      for m in MACHINES))


def capacity():
    for m in present:
        cap = data[m]["capacity"]
        body = ""
        for v in VERSIONS + EXTRA:
            c = cap.get(v)
            if not c:
                continue
            row = [same(c.get(ph, {}).get("accepted", [])) for ph in ("100", "900", "40", "300", "nginx")]
            idle = spread(c.get("idle", {}).get("mem_kb", []), 0)
            full = spread(c.get("100", {}).get("mem_full_kb", []), 0)
            after = spread(c.get("nginx", {}).get("mem_after_drain_kb", []), 0)
            ok = all(all(c.get(ph, {}).get("drained_ok", [False])) for ph in ("100", "900", "40", "300", "nginx"))
            label = "v2, all features on" if v == "v2-all" else v
            body += f"{label} & " + " & ".join(row) + f" & {idle} & {full} & {after} & {'yes' if ok else 'no'}" + r" \\" + "\n"
        w(f"capacity_{m}.tex", body)
        w(f"capacity_{m}_measure.tex", (cap.get("v2", {}).get("measure") or "—").replace("_", r"\_"))


def combined():
    """the features working together: v2 as shipped vs v2 with every allocator feature on"""
    def frac(r):
        return "—" if not r else f"{r[0]}/{r[0] + r[1]}"

    def e2e_frac(m, v):
        r = data[m]["e2e"].get(v, {}) if data[m] else {}
        return "—" if not r else f"{sum(ok for ok, _ in r.values())}/{len(r)}"

    def cap(m, v, ph, key):
        c = data[m]["capacity"].get(v, {}) if data[m] else {}
        return same(c.get(ph, {}).get(key, [])) if c.get(ph, {}).get(key) else "—"

    rows = [("Allocator stress test, checks passed",
             lambda m, v: frac(data[m]["test_apool"].get("random stress, " + ("all features" if v == "v2-all" else "daemon features")))
             if data[m] else "—"),
            ("End-to-end checks passed", e2e_frac),
            ("100 B tasks that fit", lambda m, v: cap(m, v, "100", "accepted")),
            ("nginx lines that fit", lambda m, v: cap(m, v, "nginx", "accepted")),
            ("Memory after the last burst (KB)", lambda m, v: spread(data[m]["capacity"].get(v, {}).get("nginx", {}).get("mem_after_drain_kb", []), 0)
             if data[m] and data[m]["capacity"].get(v) else "—")]
    body = ""
    for label, fn in rows:
        cells = [fn(m, v) for m in MACHINES for v in ("v2", "v2-all")]
        body += f"{label} & " + " & ".join(cells) + r" \\" + "\n"
    w("combined.tex", body)


def throughput():
    body = ""
    for mi, m in enumerate(MACHINES):
        if mi:
            body += "\\midrule\n"
        for vi, v in enumerate(VERSIONS):
            cl = data[m]["closed"].get(v, []) if data[m] else []
            op = data[m]["open"].get(v, []) if data[m] else []
            cells = [spread([x["thr"] / 1000 for x in cl], 1, "k") if cl else "—",
                     spread([x["p50"] for x in op], 0) if op else "—",
                     spread([x["p99"] for x in op], 0) if op else "—",
                     spread([x["p999"] for x in op], 0) if op else "—"]
            label = NAME[m] if vi == 0 else ""
            body += f"{label} & {v} & " + " & ".join(cells) + r" \\" + "\n"
    w("throughput.tex", body)
    errs = {m: sum(x["err"] + x["drops"] for v in VERSIONS for k in ("closed", "open") for x in data[m][k].get(v, []))
            for m in present}
    w("throughput_errors.tex", ", ".join(f"{NAME[m]}: {errs[m]}" for m in present))
    w("open_rate.tex", ", ".join(f"{NAME[m]} {data[m]['rate']:,} tasks/s" for m in present if data[m]["rate"]))


machine_rows()
unit_tests()
e2e()
ablation()
pagesize()
purge()
capacity()
combined()
throughput()
for m in MACHINES:
    if not data[m]:
        for name in (f"ablation_{m}.tex", f"capacity_{m}.tex"):
            w(name, "\\multicolumn{10}{l}{\\emph{not run yet --- see Section~\\ref{sec:server}}} \\\\\n")
        w(f"ablation_{m}_runs.tex", "0")
        w(f"capacity_{m}_measure.tex", "—")
with open(os.path.join(RES, "summary.json"), "w") as f:
    json.dump(data, f, indent=1, default=str)
print("tables written for:", ", ".join(present) or "nothing")

# one-line values used in running text and captions, as macros (captions cannot \input)
VALUES = {"openRate": "open_rate.tex", "throughputErrors": "throughput_errors.tex",
          "purgeMeasure": "purge_measure.tex", "ablationMacRuns": "ablation_mac_runs.tex",
          "capacityMacMeasure": "capacity_mac_measure.tex", "capacityServerMeasure": "capacity_server_measure.tex"}
with open(os.path.join(TAB, "values.tex"), "w") as f:
    for macro, name in VALUES.items():
        v = (read(os.path.join(TAB, name)) or "—").strip()
        f.write(f"\\newcommand{{\\{macro}}}{{{v}}}\n")
