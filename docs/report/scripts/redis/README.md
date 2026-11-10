# Quicx next to Redis

Redis is where most task queues live, so this puts the two side by side: each
in a Docker container limited to 64 MB with no swap, both sent the same
100-byte tasks at 40,000 a second by one producer (`flood.py`). Redis gets
`RPUSH` onto a list, the most compact way it can hold a queue; Quicx gets QXP
`SUBMIT`. Once both refuse (or one is killed), Redis is emptied with `LPOP`
and Quicx with 8 workers, and 10 s later the memory each container still uses
is recorded.

```sh
docs/report/scripts/redis/run.sh default    # Redis defaults (no maxmemory), Quicx defaults (8 MiB)
docs/report/scripts/redis/run.sh limit 8    # both limited to 8 MiB
docs/report/scripts/redis/run.sh limit 56   # both limited to 56 MiB
```

Needs Docker and python3. Each run writes to `docs/report/results/redis/`:
`<run>.json` (counts, errors, peak and remaining memory, `quicx status`),
`<run>-redis-memory.txt` and `<run>-quicx-memory.txt` (the container's
`memory.current`, every 50 ms, with Docker's timestamps) and `<run>.txt`
(whether Docker recorded an OOM kill).

Memory is the container's cgroup `memory.current` and `memory.peak`: what the
machine actually gave each one, not what the program reports about itself.
