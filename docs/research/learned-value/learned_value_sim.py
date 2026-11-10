#!/usr/bin/env python3
"""
Can Quicx learn what a task is worth? A simulation of the admission rule with
different sources of value.

Quicx's rule stays exactly as in src/queue.c and src/admission.c: 16 levels; a
worker takes the highest level first and the oldest task within a level; when
the budget is full, a new task evicts the oldest task of the lowest level below
its own, or is refused when there is none. Level 15 is never evicted. Every
strategy below uses that rule. They differ only in where a task's level comes
from.

A learned strategy sees what the daemon could see: sender, type, payload size,
submit time, and the score a worker returns with DONE (1 when the analysis
found something, 0 when it did not). It learns nothing about a task that was
evicted or refused, which is the selection bias a real deployment has.

Memory is counted in tasks of one size, so every eviction makes room; the room
proof (apool_free_makes_room) is tested elsewhere and does not change which
task is worth keeping.

usage: learned_value_sim.py [--seeds N] [--out FILE.json]
"""
import argparse, collections, itertools, json, math, random, statistics, sys, time

CRITICAL = 15


def to_level(p):
    """an estimate in [0, 1] to a level 1..14 (0 and 15 stay free)"""
    return max(1, min(14, 1 + int(round(p * 13))))


def logit(p):
    p = min(max(p, 1e-3), 1 - 1e-3)
    return math.log(p / (1 - p))


def sigmoid(z):
    if z < -30: return 0.0
    if z > 30: return 1.0
    return 1.0 / (1.0 + math.exp(-z))


class Task:
    __slots__ = ("t", "sender", "kind", "size", "valuable", "patch", "level", "x", "state", "action")

    def __init__(self, t, sender, kind, size, valuable, patch):
        self.t, self.sender, self.kind, self.size = t, sender, kind, size
        self.valuable, self.patch = valuable, patch
        self.level, self.x, self.state, self.action = 8, None, None, None

    @property
    def key(self):
        return (self.sender, self.kind)


# ---------------------------------------------------------------- workloads

def camera(rng, out, ids, name, fps, duration, share, patch_s, hit=0.8, false_hit=0.01,
           size_kb=60, size_signal=0.04, iid=False):
    """Frames from one camera. Sick plants come in patches that last patch_s on
    average; a frame inside a patch shows the disease with probability hit.
    share(t) is the part of the field that is sick at time t. A frame that
    shows disease is a little larger (size_signal) because spots add detail."""
    p_out = 1.0 / (patch_s * fps) if not iid else 1.0
    sick, patch = False, None
    for i in range(int(duration * fps)):
        t = (i + rng.random() * 0.2) / fps
        s = share(t)
        if iid:
            sick = rng.random() < s
            patch = next(ids) if sick else None
        else:
            if sick and rng.random() < p_out:
                sick, patch = False, None
            if not sick and rng.random() < p_out * s / (1 - s):
                sick, patch = True, next(ids)
        valuable = rng.random() < (hit if sick else false_hit)
        p = patch           # a lone spot outside a patch counts as a valuable frame, not as a patch
        size = rng.gauss(size_kb * 1024 * (1 + size_signal * valuable), size_kb * 1024 * 0.05)
        out.append(Task(t, name, "frame", size, valuable, p))


def sc_rover(rng):
    """two cameras on a field robot; the low one sees more disease"""
    out, ids, D = [], itertools.count(), 600
    camera(rng, out, ids, "front", 15, D, lambda t: 0.06, 3.0)
    camera(rng, out, ids, "low", 15, D, lambda t: 0.18, 3.0)
    return out, dict(cap=128, service=0.125)


def sc_random(rng):
    """nothing predicts value: both cameras see the same share, every frame is
    independent of the last, and size says nothing"""
    out, ids, D = [], itertools.count(), 600
    camera(rng, out, ids, "front", 15, D, lambda t: 0.10, 0, hit=1.0, false_hit=0, size_signal=0, iid=True)
    camera(rng, out, ids, "low", 15, D, lambda t: 0.10, 0, hit=1.0, false_hit=0, size_signal=0, iid=True)
    return out, dict(cap=128, service=0.125)


def sc_drift(rng):
    """halfway the cameras swap fields: the healthy one meets the disease"""
    out, ids, D = [], itertools.count(), 600
    camera(rng, out, ids, "front", 15, D, lambda t: 0.03 if t < D / 2 else 0.30, 3.0)
    camera(rng, out, ids, "low", 15, D, lambda t: 0.30 if t < D / 2 else 0.03, 3.0)
    return out, dict(cap=128, service=0.125)


SERVER_RATES = [0.02, 0.03, 0.05, 0.08, 0.10, 0.15, 0.20, 0.30, 0.45, 0.60]


def sc_server(rng):
    """one server, ten customers; how often a customer's task gives a useful
    result differs, nobody tells the queue, tasks are independent"""
    out, ids, D = [], itertools.count(), 120
    for k, rate in enumerate(SERVER_RATES):
        t = 0.0
        while True:
            t += rng.expovariate(30.0)
            if t >= D: break
            v = rng.random() < rate
            out.append(Task(t, f"c{k}", "job", rng.uniform(200, 4000), v, next(ids) if v else None))
    return out, dict(cap=1000, service=0.010)


SCENARIOS = {"rover": sc_rover, "random": sc_random, "drift": sc_drift, "server": sc_server}


# ---------------------------------------------------------------- strategies

class Strategy:
    name = "?"

    def __init__(self, rng):
        self.rng = rng

    def level(self, task, now): return 8
    def observe(self, task, score, now): pass
    def dropped(self, task, now): pass


class Fifo(Strategy):
    """no values: every task the same level"""
    name = "fifo"


class Oracle(Strategy):
    """knows the result before the analysis runs; the ceiling, not a strategy"""
    name = "oracle"

    def level(self, task, now): return 12 if task.valuable else 4


class Memory:
    """What a strategy remembers per key (sender, type): scores that fade with
    time constant tau, the plain long-run mean, when the last score came, and
    running payload-size statistics."""

    def __init__(self, tau=1.5, prior=0.1):
        self.tau, self.prior = tau, prior
        self.s, self.n, self.at = {}, {}, {}
        self.ls, self.ln = collections.Counter(), collections.Counter()
        self.gs, self.gn = 0.0, 0
        self.last = {}
        self.size = {}                       # key -> [count, mean, m2]

    def _fade(self, key, now):
        if key in self.at:
            d = math.exp(-(now - self.at[key]) / self.tau)
            self.s[key] *= d
            self.n[key] *= d
        else:
            self.s[key] = self.n[key] = 0.0
        self.at[key] = now

    def add(self, key, score, now, taken):
        """a score that arrives at now for a task submitted at taken: it says
        how the sender looked at taken, so it counts as that old"""
        self._fade(key, now)
        w = math.exp(-(now - taken) / self.tau)
        self.s[key] += score * w
        self.n[key] += w
        self.ls[key] += score
        self.ln[key] += 1
        self.gs += score
        self.gn += 1
        self.last[key] = max(self.last.get(key, -math.inf), taken)

    def glob(self):
        return (self.gs + self.prior * 10) / (self.gn + 10)

    def mean(self, key):
        return (self.ls[key] + self.glob() * 5) / (self.ln[key] + 5)

    def recent(self, key, now, strength=1.0):
        self._fade(key, now)
        m = self.mean(key)
        return (self.s[key] + m * strength) / (self.n[key] + strength)

    def beta(self, key, now, strength=1.0):
        self._fade(key, now)
        m = self.mean(key)
        return self.s[key] + m * strength, self.n[key] - self.s[key] + (1 - m) * strength

    def stale(self, key, now, tau=3.0):
        """0 just after a score from this key, towards 1 the longer it has been"""
        if key not in self.last: return 1.0
        return 1.0 - math.exp(-(now - self.last[key]) / tau)

    def size_z(self, key, size):
        c = self.size.setdefault(key, [0, 0.0, 0.0])
        c[0] += 1
        d = size - c[1]
        c[1] += d / c[0]
        c[2] += d * (size - c[1])
        if c[0] < 10: return 0.0
        sd = math.sqrt(c[2] / (c[0] - 1)) or 1.0
        return max(-4.0, min(4.0, (size - c[1]) / sd))


class SenderMean(Strategy):
    """the long-run share of useful results per sender and type"""
    name = "mean"

    def __init__(self, rng):
        super().__init__(rng); self.m = Memory()

    def level(self, task, now): return to_level(self.m.mean(task.key))
    def observe(self, task, score, now): self.m.add(task.key, score, now, task.t)


class Recent(Strategy):
    """the share of useful results in the last seconds (fading memory)"""
    name = "recent"

    def __init__(self, rng):
        super().__init__(rng); self.m = Memory()

    def estimate(self, task, now): return self.m.recent(task.key, now)
    def level(self, task, now): return to_level(self.estimate(task, now))
    def observe(self, task, score, now): self.m.add(task.key, score, now, task.t)


class RecentExplore(Recent):
    """recent, and 5 % of tasks at random get at least level 10"""
    name = "recent+explore"

    def level(self, task, now):
        lv = to_level(self.estimate(task, now))
        return max(lv, 10) if self.rng.random() < 0.05 else lv


class RecentStale(Recent):
    """recent, plus a bonus for a sender nobody has looked at for a while"""
    name = "recent+stale"

    def estimate(self, task, now):
        return min(1.0, self.m.recent(task.key, now) + 0.4 * self.m.stale(task.key, now))


class Probe:
    """Spaced exploration: one task per sender every `every` seconds gets
    level 13, so each camera is looked at regularly whatever its estimate."""
    every = 0.5

    def probe(self, task, now, lv):
        nxt = self.next_probe.get(task.key, -math.inf)
        if now >= nxt:
            self.next_probe[task.key] = now + self.every
            return 13
        return lv


class RecentProbe(Recent, Probe):
    name = "recent+probe"

    def __init__(self, rng):
        super().__init__(rng); self.next_probe = {}

    def level(self, task, now): return self.probe(task, now, to_level(self.estimate(task, now)))


class Thompson(Strategy):
    """draws the value from the fading Beta belief (Thompson sampling, a bandit)"""
    name = "thompson"

    def __init__(self, rng):
        super().__init__(rng); self.m = Memory()

    def level(self, task, now):
        a, b = self.m.beta(task.key, now)
        return to_level(self.rng.betavariate(max(a, 1e-3), max(b, 1e-3)))

    def observe(self, task, score, now): self.m.add(task.key, score, now, task.t)


class LogReg(Strategy):
    """online logistic regression on what the daemon sees at submit time:
    long-run mean, recent mean, payload size against the sender's usual size,
    and how long since the sender's last result"""
    name = "logreg"
    bonus = 0.0

    def __init__(self, rng):
        super().__init__(rng); self.m = Memory()
        self.w = [0.0, 1.0, 0.0, 0.0, 0.0]
        self.lr = 0.02

    def predict(self, x): return sigmoid(sum(a * b for a, b in zip(self.w, x)))

    def level(self, task, now):
        k = task.key
        x = [1.0, logit(self.m.mean(k)), logit(self.m.recent(k, now)), self.m.size_z(k, task.size), self.m.stale(k, now)]
        task.x = x
        p = self.predict(x)
        if self.bonus: p = min(1.0, p + self.bonus * x[4])
        return to_level(p)

    def observe(self, task, score, now):
        x = task.x
        g = score - self.predict(x)
        for i in range(len(self.w)): self.w[i] += self.lr * (g * x[i] - 1e-4 * self.w[i])
        self.m.add(task.key, score, now, task.t)


class LogRegStale(LogReg):
    name = "logreg+stale"
    bonus = 0.4


class LogRegProbe(LogReg, Probe):
    name = "logreg+probe"

    def __init__(self, rng):
        super().__init__(rng); self.next_probe = {}

    def level(self, task, now): return self.probe(task, now, LogReg.level(self, task, now))


class RLLevels(Strategy):
    """Reinforcement learning of the level itself. State: recent share of
    useful results (5 steps) and time since the sender's last result (3 steps).
    Action: level 3, 8 or 13. Reward when the task finishes: score minus the
    worker time it used (0.1); 0 when it was evicted or refused. epsilon-greedy
    with running means of the reward."""
    name = "rl-levels"
    ACTIONS = (3, 8, 13)

    def __init__(self, rng):
        super().__init__(rng); self.m = Memory()
        self.q = collections.defaultdict(lambda: [0.0, 0.0, 0.0])
        self.c = collections.defaultdict(lambda: [0, 0, 0])

    def level(self, task, now):
        r = self.m.recent(task.key, now)
        st = self.m.stale(task.key, now)
        s = (sum(r > b for b in (0.05, 0.15, 0.3, 0.6)), sum(st > b for b in (0.3, 0.7)))
        if self.rng.random() < 0.1:
            a = self.rng.randrange(3)
        else:
            q = self.q[s]; best = max(q)
            a = self.rng.choice([i for i in range(3) if q[i] == best])
        task.state, task.action = s, a
        return self.ACTIONS[a]

    def _learn(self, task, reward):
        s, a = task.state, task.action
        self.c[s][a] += 1
        self.q[s][a] += (reward - self.q[s][a]) / self.c[s][a]

    def observe(self, task, score, now):
        self._learn(task, score - 0.1)
        self.m.add(task.key, score, now, task.t)

    def dropped(self, task, now): self._learn(task, 0.0)


STRATEGIES = [Fifo, SenderMean, Recent, RecentExplore, RecentStale, RecentProbe, Thompson, LogReg, LogRegStale, LogRegProbe, RLLevels, Oracle]


# ---------------------------------------------------------------- the queue

def simulate(tasks, cfg, strat):
    cap, service = cfg["cap"], cfg["service"]
    q = [collections.deque() for _ in range(16)]
    n = 0
    cur, busy_until = None, math.inf
    done, evicted, refused = [], 0, 0

    def take():
        nonlocal n
        for L in range(15, -1, -1):
            if q[L]:
                n -= 1
                return q[L].popleft()
        return None

    def advance(now):
        nonlocal cur, busy_until
        while cur is not None and busy_until <= now:
            tf = busy_until
            done.append((cur, tf))
            strat.observe(cur, 1.0 if cur.valuable else 0.0, tf)
            cur = take()
            busy_until = tf + service if cur is not None else math.inf

    for task in tasks:
        now = task.t
        advance(now)
        task.level = strat.level(task, now)
        if cur is None:
            cur, busy_until = task, now + service
        elif n < cap:
            q[task.level].append(task); n += 1
        else:
            victim = None
            top = min(task.level + 1 if cfg.get("fresh") else task.level, CRITICAL)
            for L in range(top):
                if q[L]:
                    victim = q[L].popleft(); break
            if victim is not None:
                evicted += 1
                strat.dropped(victim, now)
                q[task.level].append(task)
            else:
                refused += 1
                strat.dropped(task, now)
    advance(math.inf)

    first = {}
    for t in tasks:
        if t.valuable and t.patch is not None and t.patch not in first: first[t.patch] = t.t
    found = {}
    waits = []
    fresh = 0
    for t, tf in done:
        if not t.valuable: continue
        waits.append(tf - t.t)
        fresh += (tf - t.t) <= 1.0
        if t.patch is not None: found[t.patch] = min(found.get(t.patch, math.inf), tf)
    vt = sum(t.valuable for t in tasks)
    delays = sorted(found[p] - first[p] for p in found)
    return {
        "valuable_done": len(waits) / vt,
        "valuable_fresh": fresh / vt,
        "patches_found": len(found) / len(first),
        "detect_p50_s": statistics.median(delays) if delays else None,
        "valuable_wait_p50_s": statistics.median(waits) if waits else None,
        "done": len(done), "evicted": evicted, "refused": refused,
        "tasks": len(tasks), "valuable": vt, "patches": len(first),
    }


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--seeds", type=int, default=5)
    ap.add_argument("--out", default=None)
    ap.add_argument("--only", default=None, help="comma-separated scenarios")
    ap.add_argument("--fresh", action="store_true",
                    help="rule variant: when no lower level exists, evict the oldest task of the same level")
    a = ap.parse_args()
    names = a.only.split(",") if a.only else list(SCENARIOS)
    res = {"seeds": a.seeds, "rule": "value+fresh" if a.fresh else "value", "scenarios": {}}
    t0 = time.time()
    for sc in names:
        rows = {}
        for S in STRATEGIES:
            runs = []
            for seed in range(a.seeds):
                tasks, cfg = SCENARIOS[sc](random.Random(1000 + seed))
                cfg["fresh"] = a.fresh
                tasks.sort(key=lambda t: t.t)
                runs.append(simulate(tasks, cfg, S(random.Random(7 + seed))))
            agg = {}
            for k in runs[0]:
                vals = [r[k] for r in runs if r[k] is not None]
                agg[k] = {"mean": statistics.fmean(vals), "min": min(vals), "max": max(vals)} if vals else None
            rows[S.name] = agg
            m = agg
            print(f"{sc:7s} {S.name:15s} valuable {m['valuable_done']['mean']*100:5.1f}%  "
                  f"fresh {m['valuable_fresh']['mean']*100:5.1f}%  patches {m['patches_found']['mean']*100:5.1f}%  "
                  f"detect {m['detect_p50_s']['mean'] if m['detect_p50_s'] else float('nan'):6.2f}s  "
                  f"wait {m['valuable_wait_p50_s']['mean']:6.2f}s  evicted {m['evicted']['mean']:8.0f}", flush=True)
        res["scenarios"][sc] = rows
    print(f"{time.time() - t0:.0f} s", file=sys.stderr)
    if a.out:
        with open(a.out, "w") as f: json.dump(res, f, indent=1)


if __name__ == "__main__":
    main()
