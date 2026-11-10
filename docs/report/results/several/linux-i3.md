# Several daemons on one Linux server, each on its own core

Intel i3-2100 (2 cores, 4 hardware threads; CPUs 0 and 2 share core 0, CPUs 1
and 3 share core 1), Ubuntu 24.04, kernel 6.8. `run_several.sh N` on the server:
each daemon has 8 workers and 2 producers x 4 connections, closed loop, 32 in
flight per connection, 10 s (2 s warmup). Three runs each.

## Each daemon on a core of its own

`PIN=1 DAEMON_CPUS="0 1" CLIENT_CPUS=2,3`: daemon i on CPU i, every producer and
worker on CPUs 2 and 3 in both cases, so the clients have the same CPUs whether
there is one daemon or two.

| daemons | per daemon | total | runs (total) |
|---:|---:|---:|---|
| 1 | 18,200 | 18,200 | 18,200, 18,000, 18,200 |
| 2 | 17,500 | 35,000 | 35,000, 35,100, 35,000 |

Two daemons on two cores do 1.92 times the work of one. During the two-daemon
run each daemon used 96-97 % of its CPU (`top`).

With the daemons not pinned but the clients still on CPUs 2 and 3, the totals
were 34,600, 35,000 and 34,800: the scheduler put the daemons on the free CPUs.

## Nothing pinned

| daemons | per daemon | total | runs (total) |
|---:|---:|---:|---|
| 1 | 18,600 | 18,600 | 18,600, 18,600, 19,000 |
| 2 | 12,000 | 24,000 | 24,200, 23,600, 24,000 |

Here the second daemon brings its own 8 workers and 2 producers, and the 4
hardware threads are shared by everything, so two daemons do 1.29 times the
work of one. What makes several daemons scale is a core that is theirs.

## Clients on another machine over Wi-Fi

`run_remote.sh`: daemon on the server, producers and workers on a Mac on the
same Wi-Fi (about 3 ms round trip). A worker takes one task per round trip, so
16 workers gave 3,200 tasks/s; 96 to 192 workers reached 10,000-10,600 tasks/s
with the daemon at about 50 % of its CPU. The network was the limit, so this
setup cannot show more than one daemon's worth of work.
