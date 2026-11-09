#!/usr/bin/env python3
"""
make_diagrams.py — the two architecture diagrams in Quicx branding.

    make_diagrams.py OUT_DIR

writes quicx-architecture-{dark,light}.svg and apool-architecture-{dark,light}.svg
(1920x1080, text as text, fonts Archivo + JetBrains Mono, icons from Lucide, ISC).

The drawing rules follow the C4 model's notation guide and review checklist
(c4model.com/diagrams/notation, c4model.com/diagrams/checklist):
  - a title that says the diagram type and its scope
  - a legend for every colour, shape, border, line style and arrow head used
  - every element has a name, a type or technology tag [in brackets] and a
    short description of what it does
  - every arrow points one way, from the sender, and carries a label
Orange marks the one thing each diagram is about: the path of a task, and a
page changing owner. Colours come from quicx-website/client/src/app/globals.css.

Every text gets a measured bounding box (fontTools when available); the script
reports text that leaves its box or overlaps other text, and exits 1 if any.
"""
import os
import re
import sys
from xml.sax.saxutils import escape

HERE = os.path.dirname(os.path.abspath(__file__))
FONTS = os.path.join(HERE, "..", "fonts")
ICONS = os.path.join(HERE, "..", "icons")
LOGO = os.path.expanduser("~/Desktop/quicx-website/client/public/quicx-logo.svg")

# ---------------------------------------------------------------- palettes
DARK = dict(bg="#0a171d", panel="#0e1e25", panel2="#132730", well="#061015", line="#27383f",
            strong="#4b5f68", text="#edf2f5", body="#c3ced5", muted="#a1b0b9", dim="#7f909a",
            accent="#ff5700", accent_text="#ff7a33", accent_soft="#2a1a10", on_accent="#0a171d",
            err="#f26b5b", word="#ffffff", ext="#16303a", warm="#3c535d", block="#8da1ab",
            block2="#5f7681", badge="#0b1a20")
LIGHT = dict(bg="#ffffff", panel="#f5f8f9", panel2="#e8eef1", well="#ffffff", line="#c5d1d7",
             strong="#7d9099", text="#0a171d", body="#22343c", muted="#4b5f68", dim="#687c85",
             accent="#ff5700", accent_text="#c94401", accent_soft="#fff0e6", on_accent="#0a171d",
             err="#c0392b", word="#0a171d", ext="#e3eaee", warm="#aebdc4", block="#5f7681",
             block2="#93a7b1", badge="#ffffff")

SANS = "Archivo, 'Helvetica Neue', Arial, sans-serif"
MONO = "'JetBrains Mono', Menlo, monospace"

# ---------------------------------------------------------------- text width
try:
    from fontTools.ttLib import TTFont

    def _metrics(path):
        f = TTFont(path)
        return f.getBestCmap(), f["hmtx"], f["head"].unitsPerEm

    _SANS_M = _metrics(os.path.join(FONTS, "Archivo[wdth,wght].ttf"))
    MEASURED = True
except Exception:
    _SANS_M = None
    MEASURED = False


def text_width(s, size, mono=False, weight=400):
    if mono:
        return len(s) * 0.6 * size
    if _SANS_M:
        cmap, hmtx, upm = _SANS_M
        w = 0
        for ch in s:
            g = cmap.get(ord(ch))
            w += hmtx[g][0] if g in hmtx.metrics else upm * 0.55
        w = w / upm * size
    else:
        w = len(s) * 0.53 * size
    return w * (1.0 + 0.10 * max(0, (weight - 400) / 300))


_ICON_CACHE = {}


def icon_body(name):
    if name not in _ICON_CACHE:
        s = open(os.path.join(ICONS, name + ".svg")).read()
        start = s.index(">", s.index("<svg")) + 1
        _ICON_CACHE[name] = re.sub(r"\s+", " ", s[start:s.rindex("</svg>")]).strip()
    return _ICON_CACHE[name]


# ---------------------------------------------------------------- drawing
class Canvas:
    def __init__(self, pal, w=1920, h=1080):
        self.p, self.w, self.h = pal, w, h
        self.items = []
        self.texts = []      # (x0, y0, x1, y1, label, container)
        self.boxes = {}
        self.items.append(f'<rect width="{w}" height="{h}" fill="{pal["bg"]}"/>')

    def c(self, k):
        return self.p.get(k, k)

    def rect(self, x, y, w, h, fill="panel", stroke="line", sw=1.5, rx=6, dash=None, name=None, opacity=None):
        d = f' stroke-dasharray="{dash}"' if dash else ""
        o = f' fill-opacity="{opacity}"' if opacity is not None else ""
        st = f' stroke="{self.c(stroke)}" stroke-width="{sw}"' if stroke else ""
        fl = "none" if fill is None else self.c(fill)
        self.items.append(f'<rect x="{x}" y="{y}" width="{w}" height="{h}" rx="{rx}" fill="{fl}"{o}{st}{d}/>')
        if name:
            self.boxes[name] = (x, y, x + w, y + h)

    def circle(self, x, y, r, fill="panel", stroke=None, sw=1.5):
        st = f' stroke="{self.c(stroke)}" stroke-width="{sw}"' if stroke else ""
        self.items.append(f'<circle cx="{x}" cy="{y}" r="{r}" fill="{self.c(fill)}"{st}/>')

    def text(self, x, y, s, size=14, mono=False, weight=400, color="body", anchor="start", within=None, check=True):
        fam = MONO if mono else SANS
        self.items.append(
            f'<text x="{x}" y="{y}" font-family="{fam}" font-size="{size}" font-weight="{weight}" '
            f'fill="{self.c(color)}" text-anchor="{anchor}">{escape(s)}</text>')
        w = text_width(s, size, mono, weight)
        x0 = x if anchor == "start" else x - w / 2 if anchor == "middle" else x - w
        if check:
            self.texts.append((x0, y - 0.74 * size, x0 + w, y + 0.22 * size, s, within))
        return w

    def label(self, x, y, s, size=12.5, color="muted", anchor="middle", mono=True, weight=400):
        """an arrow label with a knock-out background so it reads on top of lines"""
        w = text_width(s, size, mono, weight)
        x0 = x if anchor == "start" else x - w / 2 if anchor == "middle" else x - w
        self.items.append(f'<rect x="{x0 - 5:.1f}" y="{y - 0.82 * size:.1f}" width="{w + 10:.1f}" '
                          f'height="{size * 1.18:.1f}" rx="3" fill="{self.c("bg")}"/>')
        self.text(x, y, s, size=size, mono=mono, color=color, anchor=anchor, weight=weight)

    def path(self, d, color="strong", sw=1.8, arrow=None, dash=None, start_arrow=False):
        mk = {"strong": "mS", "accent": "mA", "body": "mB", "muted": "mM"}
        a = f' marker-end="url(#{mk[arrow]})"' if arrow else ""
        s = f' marker-start="url(#{mk[arrow]})"' if (arrow and start_arrow) else ""
        ds = f' stroke-dasharray="{dash}"' if dash else ""
        self.items.append(f'<path d="{d}" fill="none" stroke="{self.c(color)}" stroke-width="{sw}" '
                          f'stroke-linejoin="round" stroke-linecap="round"{ds}{a}{s}/>')

    def arrow(self, pts, color="strong", sw=1.8, dash=None, both=False):
        d = "M" + " L".join(f"{x},{y}" for x, y in pts)
        self.path(d, color, sw, arrow=color if color in ("strong", "accent", "body", "muted") else "strong",
                  dash=dash, start_arrow=both)

    def flow(self, pts, sw=2.6):
        """the orange task path"""
        self.arrow(pts, color="accent", sw=sw)

    def icon(self, name, x, y, size=24, color="text", sw=1.8):
        sc = size / 24
        self.items.append(
            f'<g transform="translate({x:.1f},{y:.1f}) scale({sc:.4f})" fill="none" stroke="{self.c(color)}" '
            f'stroke-width="{sw / sc * sc:.2f}" stroke-linecap="round" stroke-linejoin="round">{icon_body(name)}</g>')

    def badge(self, name, x, y, s=46, color="text", fill="badge", stroke="line", isize=26):
        self.rect(x, y, s, s, fill=fill, stroke=stroke, sw=1.2, rx=10)
        self.icon(name, x + (s - isize) / 2, y + (s - isize) / 2, size=isize, color=color)

    def cells(self, x, y, items, h=28, size=12, fill="well", stroke="line", color="text", sw=1.2, within=None):
        """items: list of (label, width[, fill, color]). returns end x"""
        cx = x
        for it in items:
            label, w = it[0], it[1]
            f = it[2] if len(it) > 2 else fill
            col = it[3] if len(it) > 3 else color
            self.rect(cx, y, w, h, fill=f, stroke=stroke, sw=sw, rx=0)
            if label:
                self.text(cx + w / 2, y + h / 2 + size * 0.36, label, size=size, mono=True, color=col,
                          anchor="middle", within=(cx, y, cx + w, y + h))
            cx += w
        return cx

    def step(self, x, y, n, r=13):
        self.items.append(f'<circle cx="{x}" cy="{y}" r="{r + 3}" fill="{self.c("bg")}"/>')
        self.items.append(f'<circle cx="{x}" cy="{y}" r="{r}" fill="{self.c("accent")}"/>')
        self.text(x, y + 4.8, str(n), size=14, weight=700, color="on_accent", anchor="middle", check=False)
        self.texts.append((x - r, y - r, x + r, y + r, f"step {n}", None))

    def pill(self, cx, y, s, size=11.5, anchor="middle"):
        w = text_width(s, size, mono=True) + 18
        x0 = cx - w / 2 if anchor == "middle" else cx if anchor == "start" else cx - w
        self.rect(x0, y, w, 21, fill="panel2", stroke="line", sw=1, rx=10.5)
        self.text(x0 + w / 2, y + 14.6, s, size=size, mono=True, color="muted", anchor="middle",
                  within=(x0, y, x0 + w, y + 21))
        return w

    def tile(self, x, y, w, h, icon, name, tech, desc=(), kind="comp", key=None, desc_y=None):
        """an element: icon badge, name, [technology], short description"""
        fill, stroke = ("panel", "strong") if kind == "comp" else ("ext", "line")
        self.rect(x, y, w, h, fill=fill, stroke=stroke, sw=1.6, rx=8, name=key)
        B = (x, y, x + w, y + h)
        self.badge(icon, x + 14, y + 14)
        self.text(x + 72, y + 35, name, size=19, weight=600, color="text", within=B)
        if tech:
            self.text(x + 72, y + 55, tech, size=12, mono=True, color="muted", within=B)
        dy = desc_y if desc_y is not None else y + 88
        for i, s in enumerate(desc):
            self.text(x + 16, dy + 21 * i, s, size=14.5, color="body", within=B)
        return B

    def page(self, x, y, s, kind, grid=0, hl=False):
        """one 16 KiB page. kind: used / warm / cold. grid: blocks per side (finer = smaller blocks)"""
        if kind == "cold":
            self.rect(x, y, s, s, fill=None, stroke="strong", sw=1.2, rx=3, dash="3 3")
            return
        if kind == "warm":
            self.rect(x, y, s, s, fill="warm", stroke="warm", sw=1.2, rx=3)
            return
        self.rect(x, y, s, s, fill="accent_soft" if hl else "panel2", stroke="accent" if hl else "strong",
                  sw=2 if hl else 1.2, rx=3)
        if grid:
            pad, gap = 4, 1.6 if grid > 4 else 2.4
            cell = (s - 2 * pad - gap * (grid - 1)) / grid
            for r in range(grid):
                for q in range(grid):
                    col = "accent" if hl and (r, q) == (0, 0) else ("block" if (r * grid + q) % 5 != 3 else "block2")
                    self.items.append(
                        f'<rect x="{x + pad + q * (cell + gap):.2f}" y="{y + pad + r * (cell + gap):.2f}" '
                        f'width="{cell:.2f}" height="{cell:.2f}" rx="0.8" fill="{self.c(col)}"/>')

    def logo(self, x, y, height):
        src = open(LOGO).read()
        paths = re.findall(r'<path[^>]*?d="([^"]+)"[^>]*?fill="([^"]+)"', src, re.S)
        s = height / 864.0
        g = [f'<g transform="translate({x - 176 * s:.2f},{y - 200 * s:.2f}) scale({s:.5f})">']
        for d, fill in paths:
            col = self.c("word") if fill.lower() in ("white", "#ffffff", "#fff") else self.c("accent")
            g.append(f'<path fill-rule="evenodd" clip-rule="evenodd" d="{d}" fill="{col}"/>')
        g.append("</g>")
        self.items.append("".join(g))
        self.texts.append((x, y, x + 2590 * s, y + height, "logo", None))

    def header(self, title, subtitle, corner):
        self.logo(60, 42, 40)
        self.text(self.w / 2, 64, title, size=30, weight=700, color="text", anchor="middle")
        self.text(self.w / 2, 96, subtitle, size=16.5, color="body", anchor="middle")
        self.text(self.w - 60, 64, corner, size=13, mono=True, color="muted", anchor="end")

    def svg(self, title, desc):
        p = self.p
        defs = ["<defs>"]
        for mid, col in (("mS", p["strong"]), ("mA", p["accent"]), ("mB", p["body"]), ("mM", p["muted"])):
            defs.append(f'<marker id="{mid}" viewBox="0 0 10 10" refX="8.5" refY="5" markerWidth="10" markerHeight="10" '
                        f'markerUnits="userSpaceOnUse" orient="auto-start-reverse"><path d="M0,1 L10,5 L0,9 z" fill="{col}"/></marker>')
        defs.append("</defs>")
        return (f'<svg xmlns="http://www.w3.org/2000/svg" width="{self.w}" height="{self.h}" '
                f'viewBox="0 0 {self.w} {self.h}" role="img" aria-label="{escape(title)}">'
                f'<title>{escape(title)}</title><desc>{escape(desc)}</desc>'
                f'<!-- icons: Lucide (ISC licence, see icons/LICENSE-lucide.txt) -->'
                + "".join(defs) + "".join(self.items) + "</svg>\n")

    def problems(self):
        out = []
        for (x0, y0, x1, y1, s, box) in self.texts:
            if box and (x0 < box[0] - 0.5 or x1 > box[2] + 0.5 or y0 < box[1] - 0.5 or y1 > box[3] + 0.5):
                out.append(f"leaves its box: '{s}' x {x0:.0f}-{x1:.0f} in {box[0]:.0f}-{box[2]:.0f}")
            if x0 < 0 or x1 > self.w or y0 < 0 or y1 > self.h:
                out.append(f"off canvas: '{s}'")
        t = self.texts
        for i in range(len(t)):
            for j in range(i + 1, len(t)):
                a, b = t[i], t[j]
                if a[0] < b[2] - 1 and b[0] < a[2] - 1 and a[1] < b[3] - 1 and b[1] < a[3] - 1:
                    out.append(f"overlap: '{a[4]}' and '{b[4]}'")
        return out


# ---------------------------------------------------------------- legend
def legend(c, x, y, w, rows, title="Legend"):
    """rows: (kind, text). kinds: flow, call, comp, ext, boundary, step, tech, used, warm, cold, hl, pill, err"""
    h = 46 + 31 * len(rows)
    c.rect(x, y, w, h, fill="panel", stroke="line", rx=8, name="legend")
    B = (x, y, x + w, y + h)
    c.text(x + 16, y + 28, title, size=16, weight=600, color="text", within=B)
    for i, (kind, s) in enumerate(rows):
        yy = y + 56 + 31 * i
        gx = x + 16
        if kind == "flow":
            c.flow([(gx, yy), (gx + 44, yy)])
        elif kind == "call":
            c.arrow([(gx, yy), (gx + 44, yy)])
        elif kind == "purge":
            c.arrow([(gx, yy), (gx + 44, yy)], dash="5 4")
        elif kind in ("comp", "ext"):
            c.rect(gx + 4, yy - 10, 36, 20, fill="panel" if kind == "comp" else "ext",
                   stroke="strong" if kind == "comp" else "line", sw=1.6, rx=4)
        elif kind == "boundary":
            c.rect(gx + 4, yy - 10, 36, 20, fill=None, stroke="strong", sw=1.4, rx=4, dash="6 4")
        elif kind == "step":
            c.items.append(f'<circle cx="{gx + 22}" cy="{yy}" r="11" fill="{c.c("accent")}"/>')
            c.text(gx + 22, yy + 4.5, "n", size=13, weight=700, color="on_accent", anchor="middle", check=False)
        elif kind == "tech":
            c.text(gx + 22, yy + 4.5, "[ ]", size=13, mono=True, color="muted", anchor="middle")
        elif kind == "pill":
            c.rect(gx + 2, yy - 9, 40, 18, fill="panel2", stroke="line", sw=1, rx=9)
        elif kind == "err":
            c.text(gx + 22, yy + 4.5, "ERR", size=11, mono=True, weight=700, color="err", anchor="middle")
        elif kind in ("used", "warm", "cold", "hl"):
            if kind == "used":
                c.page(gx + 4, yy - 10, 20, "used", grid=3)
                c.page(gx + 26, yy - 10, 20, "used", grid=2)
            else:
                c.page(gx + 12, yy - 10, 20, "used" if kind == "hl" else kind, grid=2 if kind == "hl" else 0,
                       hl=kind == "hl")
        c.text(x + 76, yy + 5, s, size=13.5, color="body", within=B)
    return B


# ================================================================ diagram 1
def quicx(pal):
    c = Canvas(pal)
    c.header("Quicx · Component diagram",
             "Scope: the quicxd daemon with the adaptive pool (apool v2), and the programs that talk to it",
             "adaptive-pool · Nov 2026")

    # ---- the process boundary
    c.rect(370, 132, 1180, 918, fill=None, stroke="strong", sw=1.6, rx=10, dash="8 6", name="proc")
    c.icon("cpu", 392, 146, size=22, color="muted")
    c.text(424, 164, "quicxd", size=18, mono=True, weight=700, color="text")
    c.text(500, 164, "[daemon process · C17 · one thread · no locks]", size=13, mono=True, color="muted")

    # ---- network I/O: event loop + protocol (one tile, two parts)
    c.rect(400, 184, 1120, 116, fill="panel", stroke="strong", sw=1.6, rx=8, name="io")
    IO = c.boxes["io"]
    c.badge("refresh-cw", 414, 198)
    c.text(472, 219, "Event loop", size=19, weight=600, color="text", within=IO)
    c.text(472, 239, "[event_loop.c · kqueue / epoll]", size=12, mono=True, color="muted", within=IO)
    c.text(416, 280, "one thread waits on every socket", size=14.5, within=IO)
    c.path("M800,196 L800,288", color="line", sw=1.2)
    c.badge("binary", 816, 198)
    c.text(874, 219, "QXP protocol", size=19, weight=600, color="text", within=IO)
    c.text(874, 239, "[protocol.c]", size=12, mono=True, color="muted", within=IO)
    c.text(818, 280, "encodes and decodes frames", size=14.5, within=IO)
    fx = 1104
    c.text(fx, 210, "one frame", size=12, mono=True, color="muted")
    end = c.cells(fx, 220, [("ver", 54), ("type", 58), ("length", 82), ("body", 170)], h=30, size=12.5)
    c.text(fx + 27, 268, "1 B", size=11.5, mono=True, color="dim", anchor="middle")
    c.text(fx + 83, 268, "1 B", size=11.5, mono=True, color="dim", anchor="middle")
    c.text(fx + 153, 268, "4 B", size=11.5, mono=True, color="dim", anchor="middle")
    c.text(fx + 279, 268, "0 – 1,024 B", size=11.5, mono=True, color="dim", anchor="middle")
    c.text(fx + 97, 288, "6 B header", size=11.5, mono=True, color="muted", anchor="middle")
    c.path(f"M{fx},276 L{fx},280 L{fx + 194},280 L{fx + 194},276", color="strong", sw=1)

    # ---- the middle row: server -> daemon -> queue -> dispatcher
    Y, H, W = 392, 150, 230
    xs = [410, 700, 990, 1280]
    c.tile(xs[0], Y, W, H, "plug", "Server", "[server.c]",
           ["accepts connections,", "reads and checks frames"], key="server")
    c.tile(xs[1], Y, W, H, "workflow", "Daemon", "[daemon.c]",
           ["acts on every message:", "queue, reply, register"], key="daemon")
    c.tile(xs[2], Y, W, H, "list-ordered", "Queue", "[queue.c · FIFO]", [], key="queue")
    qy = Y + 92
    for i, n in enumerate(("#41", "#42", "#43")):
        bx = xs[2] + 16 + i * 70
        c.rect(bx, qy, 52, 30, fill="accent_soft" if i == 0 else "well", stroke="accent" if i == 0 else "line",
               sw=1.4, rx=4)
        c.text(bx + 26, qy + 20, n, size=12.5, mono=True, color="text", anchor="middle")
        if i < 2:
            c.arrow([(bx + 52, qy + 15), (bx + 68, qy + 15)], sw=1.4)
    c.text(xs[2] + 16, qy + 50, "tasks wait in order", size=14.5, within=c.boxes["queue"])
    c.tile(xs[3], Y, W, H, "send", "Dispatcher", "[dispacher.c]",
           ["gives the oldest task", "to an idle worker"], key="disp")

    # ---- second row: state the daemon keeps
    c.tile(700, 620, 150, 110, "chart-column", "Stats", "[stats.c]", ["counters"], key="stats", desc_y=702)
    c.tile(1060, 620, 220, 110, "users", "Worker pool", "[workers.c]", ["idle and busy"], key="wpool", desc_y=702)

    # ---- memory
    c.tile(410, 812, 170, 218, "shield-check", "Ctrl pool", "[apool]", [], key="ctrl")
    for i, (lab, g) in enumerate((("16 B", 5), ("32 B", 4), ("32 B", 4))):
        c.page(426 + i * 48, 892, 40, "used", grid=g)
        c.text(446 + i * 48, 950, lab, size=11.5, mono=True, color="muted", anchor="middle")
    c.text(426, 982, "3 pages:", size=14.5, within=c.boxes["ctrl"])
    c.text(426, 1004, "connections, workers", size=14.5, within=c.boxes["ctrl"])

    c.tile(610, 812, 900, 218, "memory-stick", "Task pool", "[apool v2 · 508 pages of 16 KiB]", [], key="tpool")
    TP = c.boxes["tpool"]
    seq = [("used", 6, "176"), ("used", 6, "176"), ("used", 3, "976"), ("used", 6, "176"),
           ("hl", 3, "976"), ("warm", 0, ""), ("used", 6, "176"), ("used", 3, "976"), ("warm", 0, ""),
           ("used", 6, "176"), ("warm", 0, ""), ("cold", 0, ""), ("cold", 0, ""), ("cold", 0, "")]
    px0, ps, pg = 628, 36, 8
    for i, (k, g, lab) in enumerate(seq):
        x = px0 + i * (ps + pg)
        c.page(x, 892, ps, "used" if k == "hl" else k, grid=g, hl=(k == "hl"))
        if lab:
            c.text(x + ps / 2, 946, lab, size=11, mono=True, color="accent_text" if k == "hl" else "muted",
                   anchor="middle")
    lastx = px0 + len(seq) * (ps + pg)
    c.text(lastx + 4, 916, "…", size=16, color="muted")
    c.text(628, 982, "a page holds one block size while it has live blocks;", size=14.5, within=TP)
    c.text(628, 1004, "once empty, any size can take it", size=14.5, within=TP)
    # one task = one block
    bx = 1300
    c.text(bx, 880, "one task = one block", size=13, mono=True, color="accent_text")
    c.cells(bx, 890, [("Task 64 B", 84, "well", "text"), ("payload", 92, "accent_soft", "text")], h=30, size=12,
            stroke="accent", sw=1.4)
    c.text(bx, 940, "no block header", size=12, mono=True, color="dim")
    hlx = px0 + 4 * (ps + pg) + ps / 2
    c.path(f"M{hlx},892 L{hlx},876 L{bx - 12},876 L{bx - 12},905 L{bx - 2},905", color="accent", sw=1.4,
           dash="4 3")

    # ---- external programs
    c.tile(50, 184, 250, 150, "app-window", "Producers", "[your apps · TCP]",
           ["send tasks, get OK + id,", "then DONE or FAILED"], kind="ext", key="prod")
    c.tile(50, 420, 250, 124, "square-terminal", "quicx CLI", "[unix socket]",
           ["status and stats"], kind="ext", key="cli")
    c.tile(1620, 184, 250, 150, "cog", "Workers", "[your processes · TCP]",
           ["run one task at a time,", "then report back"], kind="ext", key="work")

    # ---- relationships (grey: from the sender)
    c.arrow([(400, 262), (302, 262)])
    c.label(351, 254, "replies")
    c.arrow([(302, 482), (352, 482), (352, 288), (398, 288)])
    c.label(327, 470, "STATS", anchor="middle")
    c.arrow([(1620, 290), (1522, 290)])
    c.label(1571, 282, "READY, DONE")
    c.arrow([(815, 392), (815, 302)])
    c.label(815, 352, "replies")
    c.arrow([(470, 542), (470, 810)])
    c.label(470, 760, "connection")
    c.arrow([(780, 542), (780, 618)])
    c.label(780, 584, "counts")
    c.arrow([(890, 542), (890, 676), (1058, 676)])
    c.label(974, 668, "READY: idle")
    c.arrow([(1330, 542), (1330, 676), (1282, 676)])
    c.label(1330, 600, "takes idle")

    # ---- the task path (orange)
    c.flow([(302, 222), (398, 222)])
    c.label(366, 210, "SUBMIT", color="accent_text")
    c.step(320, 222, 1)
    c.flow([(530, 302), (530, 390)])
    c.label(566, 350, "frame", color="accent_text")
    c.flow([(640, 467), (698, 467)])
    c.label(669, 452, "SUBMIT", color="accent_text", size=11.5)
    c.flow([(930, 467), (988, 467)])
    c.step(959, 467, 3)
    c.label(959, 438, "push", color="accent_text")
    c.flow([(1220, 467), (1278, 467)])
    c.step(1249, 467, 4)
    c.label(1249, 438, "pop", color="accent_text")
    c.flow([(620, 542), (620, 810)])
    c.step(620, 680, 2)
    c.label(620, 720, "alloc", color="accent_text")
    c.flow([(1440, 390), (1440, 302)])
    c.step(1440, 346, 5)
    c.label(1478, 351, "TASK", color="accent_text")
    c.flow([(1522, 236), (1618, 236)])
    c.label(1571, 228, "TASK", color="accent_text")
    c.flow([(1460, 542), (1460, 810)])
    c.step(1460, 680, 6)
    c.label(1460, 720, "free", color="accent_text")

    # ---- the task path, in words
    c.rect(1620, 392, 250, 338, fill="panel", stroke="line", rx=8, name="steps")
    S = c.boxes["steps"]
    c.text(1636, 420, "One task, step by step", size=16, weight=600, color="text", within=S)
    for i, s in enumerate(["SUBMIT arrives", "one block from the pool", "pushed on the queue",
                           "popped for an idle worker", "sent to it as TASK", "block freed right away"]):
        yy = 460 + 42 * i
        c.items.append(f'<circle cx="{1648}" cy="{yy - 5}" r="11" fill="{c.c("accent")}"/>')
        c.text(1648, yy - 0.5, str(i + 1), size=13, weight=700, color="on_accent", anchor="middle", check=False)
        c.text(1670, yy, s, size=14.5, within=S)
    c.text(1636, 716, "at-most-once delivery", size=12.5, mono=True, color="muted", within=S)

    # ---- numbers at a glance
    c.rect(1620, 812, 250, 218, fill="panel", stroke="line", rx=8, name="facts")
    F = c.boxes["facts"]
    for i, (big, small) in enumerate((("1", "thread"), ("8 MiB", "budget"), ("6 B", "header"), ("1 KiB", "max body"))):
        cx, cy = 1636 + (i % 2) * 120, 830 + (i // 2) * 100
        c.text(cx, cy + 40, big, size=30, weight=700, color="text", within=F)
        c.text(cx, cy + 66, small, size=13, mono=True, color="muted", within=F)

    # ---- legend
    legend(c, 50, 628, 250, [("flow", "path of one task"), ("call", "message or call"),
                              ("comp", "component of quicxd"), ("ext", "external program"),
                              ("boundary", "process boundary"), ("tech", "file or technology"),
                              ("used", "page: blocks in use"), ("warm", "page: empty, kept"),
                              ("cold", "page: memory returned")])
    return c


# ================================================================ diagram 2
def apool(pal):
    c = Canvas(pal)
    c.header("apool v2 · Allocator diagram",
             "Scope: one pool (the task pool, 8 MiB) · quicx/src/apool.c, 341 lines of C",
             "adaptive-pool · Nov 2026")

    # ---- 1. the mapping ------------------------------------------------
    c.rect(50, 122, 1820, 208, fill="panel", stroke="line", rx=8, name="map")
    M = c.boxes["map"]
    c.badge("layers", 66, 136)
    c.text(124, 157, "One mapping", size=19, weight=600, color="text", within=M)
    c.text(124, 177, "[mmap once · 8,339,328 B · never grown, never moved]", size=12, mono=True, color="muted",
           within=M)
    seq = ["176", "176", "976", "176", "hl", "warm", "176", "976", "warm", "176", "warm", "176", "976",
           "cold", "cold", "cold", "cold", "cold", "cold", "cold", "cold", "cold", "cold"]
    px0, ps, gap = 70, 46, 7
    for i, k in enumerate(seq):
        x = px0 + i * (ps + gap)
        if k in ("176", "976"):
            c.page(x, 206, ps, "used", grid=6 if k == "176" else 3)
        elif k == "hl":
            c.page(x, 206, ps, "used", grid=3, hl=True)
        else:
            c.page(x, 206, ps, k)
        c.text(x + ps / 2, 270, str(i), size=11, mono=True, color="accent_text" if k == "hl" else "dim",
               anchor="middle")
    lx = px0 + len(seq) * (ps + gap)
    c.text(lx + 6, 235, "…", size=18, color="muted")
    for j, n in enumerate((505, 506, 507)):
        x = lx + 36 + j * (ps + gap)
        c.page(x, 206, ps, "cold")
        c.text(x + ps / 2, 270, str(n), size=11, mono=True, color="dim", anchor="middle")
    dx0, dx1 = px0, lx + 36 + 3 * (ps + gap) - gap
    c.path(f"M{dx0},284 L{dx0},290 L{dx1},290 L{dx1},284", color="strong", sw=1)
    c.text((dx0 + dx1) / 2, 312, "508 data pages × 16 KiB", size=12.5, mono=True, color="muted", anchor="middle")
    rx0 = dx1 + 40
    c.rect(rx0, 206, 1850 - rx0, 46, fill="well", stroke="strong", sw=1.2, rx=3)
    for k in range(1, 12):
        xx = rx0 + k * (1850 - rx0) / 12
        c.path(f"M{xx:.1f},206 L{xx:.1f},252", color="line", sw=1)
    rw = (1850 - rx0) / 12
    c.rect(rx0 + 4 * rw, 206, rw, 46, fill="accent_soft", stroke="accent", sw=1.6, rx=0)
    c.text(rx0 + 4.5 * rw, 270, "4", size=11, mono=True, color="accent_text", anchor="middle")
    c.path(f"M{rx0},284 L{rx0},290 L1850,290 L1850,284", color="strong", sw=1)
    c.text((rx0 + 1850) / 2, 312, "508 page records × 32 B", size=12.5, mono=True, color="muted", anchor="middle")
    c.text(rx0, 196, "record i describes page i", size=12, mono=True, color="muted")
    c.text(1850, 157, "no block headers: a block's page is", size=13.5, anchor="end", within=M)
    c.text(1850, 177, "(address − start) / 16 KiB", size=13, mono=True, color="text", anchor="end", within=M)

    TOP, BOT = 346, 800

    # ---- 2. size classes -> their pages ----------------------------------
    c.rect(50, TOP, 640, BOT - TOP, fill="panel", stroke="strong", sw=1.6, rx=8, name="cls")
    C = c.boxes["cls"]
    c.badge("rows-3", 66, TOP + 14)
    c.text(124, TOP + 35, "Size classes", size=19, weight=600, color="text", within=C)
    c.text(124, TOP + 55, "[68 classes · 16, 32 … 1,088 B]", size=12, mono=True, color="muted", within=C)
    rows = [("16 B", []), ("…", None), ("176 B", [0, 1, 3, 6, 9, 11]), ("…", None),
            ("976 B", [2, "hl", 7, 12]), ("…", None), ("1,088 B", [])]
    yy = TOP + 84
    for name, pages in rows:
        if pages is None:
            c.text(110, yy + 13, "⋮", size=16, color="dim", anchor="middle")
            yy += 24
            continue
        c.rect(70, yy, 80, 34, fill="well", stroke="line", sw=1.2, rx=4)
        c.text(110, yy + 22, name, size=12.5, mono=True, color="text", anchor="middle")
        if not pages:
            c.text(170, yy + 22, "no page yet", size=12.5, mono=True, color="dim")
            yy += 44
            continue
        x = 170
        for k in pages:
            hl = k == "hl"
            c.page(x, yy + 3, 28, "used", grid=(3 if name == "976 B" else 5), hl=hl)
            c.text(x + 14, yy + 47, "4" if hl else str(k), size=10.5, mono=True,
                   color="accent_text" if hl else "dim", anchor="middle")
            if k != pages[-1]:
                c.arrow([(x + 28, yy + 17), (x + 44, yy + 17)], sw=1.2)
            x += 46
        c.text(x + 6, yy + 22, f"{len(pages)} pages", size=12, mono=True, color="muted")
        yy += 56
    c.text(70, BOT - 44, "a size owns only the pages it needs right now", size=14.5, within=C)
    x = 70
    for s in ("slab · Bonwick 1994", "lifetimes · LLAMA 2020", "fullest-first · Hoard (off)"):
        x += c.pill(x, BOT - 32, s, anchor="start") + 8

    # ---- 3. inside one page ----------------------------------------------
    c.rect(720, TOP, 560, BOT - TOP, fill="panel", stroke="strong", sw=1.6, rx=8, name="pg")
    P = c.boxes["pg"]
    c.badge("search-check", 736, TOP + 14)
    c.text(794, TOP + 35, "Inside page 3", size=19, weight=600, color="text", within=P)
    c.text(794, TOP + 55, "[176 B blocks · 93 fit · 16 B spare]", size=12, mono=True, color="muted", within=P)
    sy, cw, ch = TOP + 150, 32, 40
    sx = 744
    kinds = ["live", "free", "live", "live", "free", "live", "free", "live", "live", "live", "live", "live"]
    cx = {}
    for i, k in enumerate(kinds):
        x = sx + i * cw
        cx[i] = x + cw / 2
        if k == "live":
            c.rect(x + 1, sy, cw - 2, ch, fill="block", stroke=None, rx=2)
        else:
            c.rect(x + 1, sy, cw - 2, ch, fill="well", stroke="accent", sw=1.6, rx=2)
        c.text(x + cw / 2, sy + ch + 18, str(i), size=10.5, mono=True, color="dim", anchor="middle")
    ux = sx + len(kinds) * cw
    c.rect(ux + 1, sy, 1252 - ux - 16, ch, fill=None, stroke="strong", sw=1, rx=2, dash="3 3")
    c.text((ux + 1252 - 16) / 2, sy + ch / 2 + 4.5, "12 … 92", size=12, mono=True, color="dim", anchor="middle")
    c.rect(1252 - 14, sy, 12, ch, fill="line", stroke=None, rx=1)
    # free list: head -> 6 -> 4 -> 1 -> end (last freed first)
    c.label(sx + 24, sy - 66, "free_list", color="accent_text", anchor="start")
    c.path(f"M{sx + 96},{sy - 70} L{cx[6]},{sy - 70} L{cx[6]},{sy - 4}", color="accent", sw=1.8, arrow="accent")
    for a, b in ((6, 4), (4, 1)):
        c.path(f"M{cx[a] - 6},{sy} C{cx[a] - 6},{sy - 34} {cx[b] + 6},{sy - 34} {cx[b] + 6},{sy - 4}",
               color="accent", sw=1.8, arrow="accent")
    c.text(cx[1] - 22, sy - 30, "end", size=11, mono=True, color="dim", anchor="middle")
    c.path(f"M{cx[1] - 6},{sy} L{cx[1] - 6},{sy - 22} L{cx[1] - 14},{sy - 22}", color="accent", sw=1.4)
    # bump
    c.arrow([(ux, sy + ch + 50), (ux, sy + ch + 6)], sw=1.6)
    c.label(ux, sy + ch + 66, "bump", anchor="middle")
    # zoom into freed block 4
    zy = sy + ch + 92
    b4 = sx + 4 * cw
    c.path(f"M{b4 + 1},{sy + ch + 26} L744,{zy}", color="strong", sw=1, dash="3 3")
    c.path(f"M{b4 + cw - 1},{sy + ch + 26} L1104,{zy}", color="strong", sw=1, dash="3 3")
    c.cells(744, zy, [("next ⊕ key", 150, "accent_soft", "text"), ("old data", 210, "well", "dim")],
            h=30, size=12, stroke="line")
    c.text(744, zy + 50, "freed block 4: its first 8 B point to the next free block", size=13, within=P)
    # key
    ky = zy + 96
    for i, (kind, s) in enumerate((("live", "in use"), ("free", "freed"), ("bump", "never used"))):
        x = 744 + i * 170
        if kind == "live":
            c.rect(x, ky - 12, 24, 16, fill="block", stroke=None, rx=2)
        elif kind == "free":
            c.rect(x, ky - 12, 24, 16, fill="well", stroke="accent", sw=1.6, rx=2)
        else:
            c.rect(x, ky - 12, 24, 16, fill=None, stroke="strong", sw=1, rx=2, dash="3 3")
        c.text(x + 32, ky + 1, s, size=13, within=P)
    c.pill(736, BOT - 32, "free list in the page · mimalloc 2019", anchor="start")
    c.text(1264, BOT - 18, "a broken link is caught", size=13, anchor="end", within=P)

    # ---- 4. page lifecycle -----------------------------------------------
    c.rect(1310, TOP, 560, BOT - TOP, fill="panel", stroke="strong", sw=1.6, rx=8, name="life")
    L = c.boxes["life"]
    c.badge("recycle", 1326, TOP + 14)
    c.text(1384, TOP + 35, "Life of a page", size=19, weight=600, color="text", within=L)
    c.text(1384, TOP + 55, "[pages change owner; blocks never move]", size=12, mono=True, color="muted", within=L)

    def state(x, y, icon, name, sub, key):
        c.rect(x, y, 150, 72, fill="panel2", stroke="strong", sw=1.4, rx=8, name=key)
        c.icon(icon, x + 12, y + 13, size=22, color="text")
        c.text(x + 42, y + 30, name, size=16, weight=600, color="text", within=c.boxes[key])
        c.text(x + 12, y + 58, sub, size=12, mono=True, color="muted", within=c.boxes[key])

    s1y, s2y = TOP + 100, TOP + 268
    state(1336, s1y, "snowflake", "cold", "no memory", "s_cold")
    state(1696, s1y, "package", "in use", "one block size", "s_used")
    state(1516, s2y, "flame", "warm", "memory kept", "s_warm")
    c.flow([(1486, s1y + 26), (1694, s1y + 26)])
    c.label(1590, s1y + 18, "take_page", color="accent_text")
    c.label(1590, s1y + 46, "if none warm", color="muted")
    c.arrow([(1781, s1y + 74), (1781, s2y + 36), (1668, s2y + 36)])
    c.label(1781, s1y + 130, "last block")
    c.label(1781, s1y + 148, "freed")
    c.flow([(1630, s2y - 2), (1716, s1y + 74)])
    c.label(1598, s1y + 120, "take_page", color="accent_text")
    c.label(1598, s1y + 138, "newest first", color="muted")
    c.arrow([(1514, s2y + 36), (1411, s2y + 36), (1411, s1y + 74)], dash="5 4")
    c.label(1411, s1y + 130, "purge")
    c.label(1411, s1y + 148, "[madvise]")
    ly = BOT - 74
    c.text(1326, ly, "> 63 warm: the oldest gives memory back", size=13.5, within=L)
    c.pill(1855, ly - 15, "jemalloc 2006", anchor="end")
    c.text(1326, ly + 34, "≤ 7 free: only a size with no page", size=13.5, within=L)
    c.text(1326, ly + 54, "may take one (the reserve)", size=13.5, within=L)
    c.pill(1855, ly + 19, "watermarks · Linux", anchor="end")

    # ---- 5. alloc and free, step by step ---------------------------------
    def lane(y, title, steps, key):
        c.rect(50, y, 1820, 86, fill="panel", stroke="line", rx=8, name=key)
        B = c.boxes[key]
        c.text(70, y + 30, title, size=17, mono=True, weight=700, color="text", within=B)
        x, w = 290, 392
        for i, (icon, s, err) in enumerate(steps):
            c.rect(x, y + 12, w - 26, 62, fill="panel2", stroke="line", sw=1.2, rx=6, name=f"{key}{i}")
            S = c.boxes[f"{key}{i}"]
            c.step(x + 22, y + 43, i + 1, r=11)
            c.icon(icon, x + 44, y + 31, size=24, color="body")
            c.text(x + 80, y + 38, s[0], size=14.5, color="text", within=S)
            c.text(x + 80, y + 60, s[1], size=12.5, mono=True, color="muted", within=S)
            if err:
                c.text(x + w - 36, y + 31, err, size=11, mono=True, weight=700, color="err", anchor="end", within=S)
            if i < len(steps) - 1:
                c.arrow([(x + w - 26, y + 43), (x + w - 2, y + 43)], sw=1.6)
            x += w

    lane(812, "alloc(size)", [
        ("hash", ("size → class", "c = (size + 15) / 16 − 1"), "SIZE"),
        ("rows-3", ("a page with room", "else take_page()"), "FULL"),
        ("list-ordered", ("pop the free list", "else bump"), ""),
        ("package", ("hand out the block", "no header · live + 1"), "")], "la")
    lane(908, "free(ptr)", [
        ("search-check", ("ptr → page", "(ptr − data) / 16 KiB"), "INVALID"),
        ("shield-check", ("check the block", "owned · on a block edge"), "DOUBLE"),
        ("list-ordered", ("push on the free list", "link XOR-ed"), ""),
        ("recycle", ("last block? page → warm", "live = 0"), "")], "lf")

    # ---- legend, one row ---------------------------------------------------
    c.rect(50, 1006, 1820, 58, fill="panel", stroke="line", rx=8, name="leg")
    G = c.boxes["leg"]
    c.text(70, 1041, "Legend", size=15, weight=600, color="text", within=G)
    x = 150
    items = [("used", "page in use (finer grid = smaller blocks)"), ("warm", "empty, memory kept"),
             ("cold", "memory returned or never used"), ("hl", "page 4, followed through"),
             ("flow", "page changes owner"), ("call", "next step / pointer"), ("purge", "memory back to the OS"),
             ("err", "error returned")]
    for kind, s in items:
        yy = 1035
        if kind == "used":
            c.page(x, yy - 10, 20, "used", grid=4)
            c.page(x + 24, yy - 10, 20, "used", grid=2)
            tx = x + 52
        elif kind in ("warm", "cold"):
            c.page(x, yy - 10, 20, kind)
            tx = x + 28
        elif kind == "hl":
            c.page(x, yy - 10, 20, "used", grid=2, hl=True)
            tx = x + 28
        elif kind in ("flow", "call", "purge"):
            if kind == "flow":
                c.flow([(x, yy), (x + 34, yy)], sw=2.2)
            else:
                c.arrow([(x, yy), (x + 34, yy)], dash="5 4" if kind == "purge" else None)
            tx = x + 44
        else:
            c.text(x, yy + 5, "ERR", size=11, mono=True, weight=700, color="err")
            tx = x + 34
        w = c.text(tx, yy + 5, s, size=13, within=G)
        x = tx + w + 30
    return c


def main():
    out = sys.argv[1] if len(sys.argv) > 1 else "."
    os.makedirs(out, exist_ok=True)
    bad = 0
    print("text widths:", "measured with fontTools" if MEASURED else "ESTIMATED (fontTools missing)")
    for name, fn, title, desc in (
            ("quicx-architecture", quicx, "Quicx component diagram",
             "Producers and workers talk to quicxd over TCP; inside it an event loop and the protocol feed the "
             "server, daemon, queue and dispatcher; tasks live in one block of the task pool from SUBMIT until "
             "they are sent as TASK."),
            ("apool-architecture", apool, "apool v2 allocator diagram",
             "One mapping of 16 KiB pages and 32 B page records; size classes own lists of pages; free blocks "
             "inside a page form an encoded free list; empty pages stay warm, then return their memory.")):
        for variant, pal in (("dark", DARK), ("light", LIGHT)):
            cv = fn(pal)
            path = os.path.join(out, f"{name}-{variant}.svg")
            with open(path, "w") as f:
                f.write(cv.svg(title, desc))
            probs = cv.problems()
            if variant == "dark":
                for p in probs:
                    print(f"  {name}: {p}")
                bad += len(probs)
            print(f"wrote {path} ({len(probs)} layout problems)")
    sys.exit(1 if bad else 0)


if __name__ == "__main__":
    main()
