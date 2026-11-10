# Can Quicx learn what a task is worth?

Value-aware admission keeps the most valuable work when the budget is full, but
someone has to say what a task is worth. A producer often can't: a camera
doesn't know which frame shows a sick plant until the model has looked at it.
This experiment asks whether the value can be learned from what the daemon
already sees, and which way of learning it works.

`learned_value_sim.py` runs Quicx's admission rule unchanged (16 levels, highest
level first, oldest first within a level, evict the oldest task of the lowest
level below the new one) and changes only where a task's level comes from. A
learned strategy sees the sender, the type, the payload size, the submit time
and the score a worker returns with DONE (1 when the analysis found something).
It never hears about a task that was evicted or refused.

```
python3 learned_value_sim.py --seeds 5 --out results/value.json
python3 learned_value_sim.py --seeds 5 --fresh --out results/value-fresh.json
```

Each row is the mean of 5 seeds; min and max are in the JSON. The whole run
takes about 10 seconds.

## Workloads

| | What it models | Load |
|---|---|---|
| rover | a field robot, two cameras at 15 frames/s; sick plants come in patches of about 3 s; the low camera sees three times more disease; a frame with disease is about 4 % larger | the worker analyses 8 frames/s, the budget holds 128 frames, 10 minutes |
| drift | the same, but halfway the cameras swap fields | same |
| random | nothing predicts value: both cameras see 10 % disease, every frame independent, size says nothing | same |
| server | one server, ten customers at 30 tasks/s each; the share of useful results differs by customer (2 % to 60 %) and nobody tells the queue | the worker does 100 tasks/s, the budget holds 1,000 tasks, 2 minutes |

## Strategies

| | Where the level comes from |
|---|---|
| fifo | nowhere: every task level 8 |
| mean | long-run share of useful results per sender and type |
| recent | the same share over the last seconds (memory fading with τ = 1.5 s) |
| +explore | 5 % of tasks at random get at least level 10 |
| +stale | a bonus for a sender with no result for a while |
| +probe | one task per sender every 0.5 s gets level 13 |
| thompson | a value drawn from the fading Beta belief per sender (Thompson sampling, a bandit) |
| logreg | online logistic regression on long-run share, recent share, payload size against the sender's usual size, time since the last result |
| rl-levels | reinforcement learning of the level itself: state is the recent share and staleness, action is level 3, 8 or 13, reward is the score minus the worker time used |
| oracle | knows the result before the analysis; the ceiling, not a strategy |

## Results: the field robot

| | Diseased frames analysed | within 1 s | Patches found | Patch found after (p50) |
|---|---|---|---|---|
| fifo | 27.7 % | 0.0 % | 93.6 % | 16.2 s |
| mean | 34.8 % | 1.1 % | 76.7 % | 16.1 s |
| recent | 30.3 % | 1.8 % | 51.1 % | 16.1 s |
| recent+explore | 62.0 % | 18.7 % | 82.6 % | 1.1 s |
| recent+probe | 66.3 % | 22.7 % | 92.7 % | 0.49 s |
| thompson | 59.3 % | 28.6 % | 90.3 % | 0.89 s |
| logreg | 53.5 % | 24.6 % | 78.8 % | 2.3 s |
| **logreg+probe** | **66.7 %** | **29.9 %** | **95.4 %** | **0.47 s** |
| rl-levels | 51.6 % | 12.7 % | 86.4 % | 4.1 s |
| oracle | 99.9 % | 36.8 % | 100 % | 0.21 s |

## What the numbers say

**Learning helps where value has a visible cause.** On the robot, value comes in
streaks: a sick plant is followed by more sick plants. Logistic regression with
probes analyses 2.4 times more diseased frames than a plain queue (66.7 % against
27.7 %), finds as many patches (95.4 % against 93.6 %), and finds them 34 times
sooner (0.47 s against 16.2 s). On the server, value follows the customer:
the long-run share per customer doubles the useful results (73.4 % against
36.4 %). That is the ceiling for anything that only knows the customer: the
worker has room for the three best customers and part of the fourth, 71.5 % of
the useful tasks.

**Learning cannot help where nothing predicts value.** On the random workload
every strategy analyses the same 27-28 % of diseased frames. Probes still cut
the wait from 16 s to under 1 s and cost nothing measurable.

**Learning without exploration is worse than not learning.** `mean` and
`recent` alone find only 51-77 % of the patches, against 94 % for a plain
queue. They lower the level of a camera that looked healthy, its frames get
evicted, no result ever comes back, and the estimate never recovers. Every
strategy that keeps looking (probes, random exploration, Thompson sampling)
avoids this. On the drift workload the gap is widest: `mean` finds 48 % of the
patches, `logreg+probe` 94 %.

**Spaced probes beat random exploration.** One probe per sender every 0.5 s
guarantees that each camera is looked at regularly. Random exploration at 5 %
reaches the same frames by chance and finds 83 % of the patches instead of 93 %.
The interval is a trade-off: every 0.25 s finds 97.7 % of the patches in 0.38 s
but the probes take the whole worker and only 40 % of diseased frames get
analysed; every 2 s the patches found fall to 90 %.

**Reinforcement learning of the level itself is weaker than estimating the
value.** `rl-levels` reaches 51.6 % on the robot and 57.2 % on the server, against
66.7 % and 73.4 %. A level is only worth something relative to the levels of
other tasks, so the reward of one choice depends on all the others; estimating
the value and leaving the ordering to the rule removes that problem.

**A score must be dated by when the task was submitted.** A result that comes
back 16 s late describes the field 16 s ago. The first version of the simulator
counted it as fresh, and `recent` then performed worse than `fifo`.

## The rule variant

`--fresh` lets a new task also evict the oldest task of its own level. It cuts
the plain queue's wait from 16 s to 4.4 s because old frames make room for new
ones, but the share of diseased frames analysed stays at 27.6 %. With the
learned strategies it changes almost nothing (`logreg+probe`: 66.1 %, 0.48 s).

## What it would take in Quicx

`logreg+probe` keeps five weights in total and, per sender, a handful of numbers
(fading sum and count, long-run sum and count, time of the last result, the
next probe time, payload size mean and variance). Each submit costs one dot
product of five numbers and an exponential, well under a microsecond in C.

1. A score in DONE: one byte after the result, 0-255, where workers that send
   nothing count as unknown and teach nothing.
2. `admission = learned`: tasks submitted without a value get the learned
   level; tasks with a value keep it, so a producer that knows still decides.
3. The per-sender state lives in the daemon's own memory, outside the task
   budget, bounded by a fixed table of senders.

Until then a producer can do the same on its own side today, with
`SUBMIT_VALUED` and the results its workers return.

## Limits

This is a simulation with modelled workloads. The patch model, the size signal
and the rates are assumptions; a real field will differ. The next step is to
replay frames recorded on the robot and to measure the C implementation on the
daemon, the way `docs/report/scripts/admission` measures value-aware admission.
