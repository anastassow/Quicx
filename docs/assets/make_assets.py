#!/usr/bin/env python3
"""
make_assets.py — the README's banner and results chart, in Quicx branding.

    python3 docs/assets/make_assets.py         (writes docs/assets/*.svg)
    docs/assets/export.sh                       (SVG -> PNG at 2x, needs rsvg-convert)

Uses the drawing code and palettes of docs/report/scripts/make_diagrams.py,
so the README, the diagrams and the website share one look. Every number in
the chart is from docs/report/results (see the report, tables 9 and 12).
"""
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "report", "scripts"))
from make_diagrams import Canvas, DARK, LIGHT  # noqa: E402

# measured, docs/report/results/{mac,server}/capacity_*.txt (medians of 3 runs)
CAPACITY_100B = {"v1.0.4": 655, "v2.0.0": 46593}          # identical on both machines
MEMORY_AFTER_BURST_KB = {                                   # after the last burst drains
    "Apple M4 Pro, macOS": {"v1.0.4": 9200, "v2.0.0": 2080},
    "Intel i3-2100, Linux": {"v1.0.4": 9928, "v2.0.0": 2736},
}


def bar(c, x, y, w, h, color):
    """a bar anchored at its left edge, rounded at the data end"""
    r = min(4, w / 2, h / 2)
    c.items.append(
        f'<path d="M{x},{y} H{x + w - r} Q{x + w},{y} {x + w},{y + r} V{y + h - r} '
        f'Q{x + w},{y + h} {x + w - r},{y + h} H{x} Z" fill="{c.c(color)}"/>')


def banner(pal):
    c = Canvas(pal, 1600, 420)
    c.logo(80, 82, 64)
    c.text(80, 238, "The task queue with a hard memory limit.", size=46, weight=700, color="text")
    c.text(80, 290, "One process, one thread, one fixed budget that is never exceeded.", size=23, color="body")
    c.text(80, 324, "v2 fits 71 times more tasks in the same 8 MiB.", size=23, color="body")
    c.text(80, 372, "v2.0.0  ·  C17  ·  macOS and Linux  ·  MIT", size=17, mono=True, color="muted")

    # the allocator's pages, as in the diagrams: in use (finer grid = smaller blocks), warm, cold
    kinds = ["u6", "u6", "u3", "u6", "hl", "w", "u6", "u3",
             "u3", "w", "u6", "u6", "u3", "c", "u6", "w",
             "u6", "u3", "c", "u6", "c", "c", "w", "c",
             "c", "u6", "c", "c", "c", "c", "c", "c"]
    s, g, x0, y0 = 46, 10, 1080, 96
    for i, k in enumerate(kinds):
        x, y = x0 + (i % 8) * (s + g), y0 + (i // 8) * (s + g)
        if k.startswith("u"):
            c.page(x, y, s, "used", grid=int(k[1]))
        elif k == "hl":
            c.page(x, y, s, "used", grid=3, hl=True)
        elif k == "w":
            c.page(x, y, s, "warm")
        else:
            c.page(x, y, s, "cold")
    return c


def results(pal):
    W, H = 1600, 640
    c = Canvas(pal, W, H)
    c.text(60, 70, "v1.0.4 → v2.0.0", size=30, weight=700, color="text")
    c.text(60, 104, "measured on an Apple M4 Pro (macOS) and an Intel Core i3-2100 (Linux), default 8 MiB budget",
           size=18, color="body")
    # legend
    lx = 60
    for name, col in (("v1.0.4  (PMAD, fixed size classes)", "block2"), ("v2.0.0  (apool v2, adaptive pages)", "accent")):
        c.rect(lx, 132, 16, 16, fill=col, stroke=None, rx=3)
        lx += 26 + c.text(lx + 26, 146, name, size=16, color="body") + 32

    def panel(x, title, sub):
        c.rect(x, 180, 720, 400, fill="panel", stroke="line", rx=10)
        c.text(x + 28, 222, title, size=21, weight=600, color="text")
        c.text(x + 28, 250, sub, size=15, color="muted")

    # A: capacity
    panel(60, "Tasks of 100 B that fit", "same count on both machines · higher is better")
    x0, scale = 210, 470 / 50000
    for i, (v, n) in enumerate(CAPACITY_100B.items()):
        y = 316 + i * 96
        c.text(88, y + 21, v, size=17, mono=True, color="body")
        w = max(3, n * scale)
        bar(c, x0, y, w, 30, "accent" if v == "v2.0.0" else "block2")
        c.text(x0 + w + 12, y + 22, f"{n:,}", size=19, weight=600, color="text")
    c.text(88, 540, "71× more in the same memory", size=17, mono=True, color="accent_text")

    # B: memory after a burst
    panel(820, "Memory held after a burst", "KB of physical memory once the queue drains · lower is better")
    x0, scale = 1000, 430 / 10000
    y = 296
    for machine, vals in MEMORY_AFTER_BURST_KB.items():
        c.text(848, y + 4, machine, size=15, color="muted")
        for v, kb in vals.items():
            y += 18
            c.text(848, y + 22, v, size=17, mono=True, color="body")
            w = kb * scale
            bar(c, x0, y, w, 28, "accent" if v == "v2.0.0" else "block2")
            c.text(x0 + w + 12, y + 21, f"{kb:,}", size=19, weight=600, color="text")
            y += 30
        y += 44
    c.text(60, 618, "Source: docs/report (raw runs in docs/report/results). macOS memory is phys_footprint, "
                    "Linux memory is resident set size.", size=14, color="dim")
    return c


# Quicx 2.0.0 on its own, docs/report/results/{mac,server} (medians of 5 runs; memory of 3)
LATENCY_US = {  # submit -> done, open loop: 40k/s on the Mac, 9k/s on the i3
    "p50": (49, 123), "p90": (64, 157), "p99": (98, 198), "p99.9": (143, 208)}
CAPACITY_V2 = [("40 B", 73146), ("100 B", 46593), ("nginx, 188 B", 30947), ("300 B", 22044), ("900 B", 8016)]
MEMORY_V2_KB = [("idle", 1056, 1796), ("queue full", 9088, 9816), ("drained", 2080, 2736)]


def v2(pal):
    W, H = 1600, 860
    c = Canvas(pal, W, H)
    c.text(60, 70, "Quicx 2.0.0, measured", size=30, weight=700, color="text")
    c.text(60, 104, "Apple M4 Pro (macOS) and Intel Core i3-2100 (Linux) · default 8 MiB budget · "
                    "C clients over loopback · medians of repeated runs", size=18, color="body")
    # headline figures
    tiles = [("81,900", "tasks / s", "end to end, one thread, M4 Pro"),
             ("49 µs", "p50", "submit → done at 40k/s, M4 Pro"),
             ("46,593", "tasks", "of 100 B in the 8 MiB budget"),
             ("2,080 KB", "held", "after a burst drains, M4 Pro")]
    for i, (big, unit, note) in enumerate(tiles):
        x = 60 + i * 375
        c.rect(x, 130, 355, 116, fill="panel", stroke="line", rx=10, name=f"t{i}")
        B = c.boxes[f"t{i}"]
        w = c.text(x + 24, 190, big, size=40, weight=700, color="text", within=B)
        c.text(x + 34 + w, 190, unit, size=17, mono=True, color="accent_text", within=B)
        c.text(x + 24, 224, note, size=15, color="muted", within=B)

    def panel(x, w, title, sub, key):
        c.rect(x, 276, w, 520, fill="panel", stroke="line", rx=10, name=key)
        c.text(x + 24, 316, title, size=20, weight=600, color="text")
        c.text(x + 24, 342, sub, size=14.5, color="muted")

    def legend(x, y):
        for j, (name, col) in enumerate((("M4 Pro", "accent"), ("i3-2100", "block2"))):
            c.rect(x + j * 120, y - 12, 14, 14, fill=col, stroke=None, rx=3)
            c.text(x + j * 120 + 22, y, name, size=14, color="body")

    # latency
    panel(60, 480, "Latency, submit → done", "µs · open loop at 40k/s (M4 Pro) and 9k/s (i3)", "pl")
    legend(84, 380)
    x0, scale, y = 160, 300 / 220, 412
    for name, (mac, lin) in LATENCY_US.items():
        c.text(84, y + 30, name, size=16, mono=True, color="body")
        for k, (val, col) in enumerate(((mac, "accent"), (lin, "block2"))):
            yy = y + k * 30
            bar(c, x0, yy, val * scale, 24, col)
            c.text(x0 + val * scale + 10, yy + 18, str(val), size=15, weight=600, color="text")
        y += 92

    # capacity
    panel(560, 480, "Tasks that fit in 8 MiB", "accepted before ERROR 0x01 · same on both machines", "pc")
    x0, scale, y = 720, 230 / 75000, 392
    for name, n in CAPACITY_V2:
        c.text(584, y + 21, name, size=15.5, mono=True, color="body")
        bar(c, x0, y, max(3, n * scale), 30, "accent")
        c.text(x0 + n * scale + 10, y + 22, f"{n:,}", size=16, weight=600, color="text")
        y += 76

    # memory over a burst
    panel(1060, 480, "Memory over one burst", "KB of physical memory of the whole daemon process", "pm")
    legend(1084, 380)
    x0, scale, y = 1200, 250 / 10000, 412
    for name, mac, lin in MEMORY_V2_KB:
        c.text(1084, y + 30, name, size=15.5, mono=True, color="body")
        for k, (val, col) in enumerate(((mac, "accent"), (lin, "block2"))):
            yy = y + k * 30
            bar(c, x0, yy, val * scale, 24, col)
            c.text(x0 + val * scale + 10, yy + 18, f"{val:,}", size=15, weight=600, color="text")
        y += 120

    c.text(60, 836, "Source: docs/report/results. Throughput and latency: median of 5 runs; capacity and memory: "
                    "median of 3. macOS memory is phys_footprint, Linux resident set size.", size=14, color="dim")
    return c


def main():
    bad = 0
    for name, fn, title, desc in (
            ("banner", banner, "Quicx", "Quicx, the task queue with a hard memory limit"),
            ("benchmarks", v2, "Quicx 2.0.0 benchmarks",
             "Quicx 2.0.0: 81,900 tasks per second at 49 µs median latency on an Apple M4 Pro, 46,593 tasks "
             "of 100 B in 8 MiB, and about 2 MB of memory held after a burst, on macOS and Linux"),
            ("results", results, "Quicx v1.0.4 vs v2.0.0",
             "v2.0.0 fits 46,593 tasks of 100 B in 8 MiB where v1.0.4 fits 655, and holds about 2 MB "
             "instead of 9 MB after a burst, on macOS and Linux")):
        for variant, pal in (("dark", DARK), ("light", LIGHT)):
            cv = fn(pal)
            with open(os.path.join(HERE, f"{name}-{variant}.svg"), "w") as f:
                f.write(cv.svg(title, desc))
            probs = cv.problems()
            for p in probs:
                print(f"  {name}-{variant}: {p}")
            bad += len(probs)
    print("assets written,", bad, "layout problems")
    sys.exit(1 if bad else 0)


if __name__ == "__main__":
    main()
