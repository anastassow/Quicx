# Value-aware admission under overload

`bench_admission.py` starts `quicxd` with a 256 KiB budget and each admission
mode, feeds it more work than its one worker can do, and counts what happens
to the valuable tasks: done (DONE reached the producer), evicted (FAILED
"evicted") or refused (ERROR 0x01), and how long the done ones waited.

```sh
make quicxd
python3 docs/report/scripts/admission/bench_admission.py ./quicxd out.json
```

The results of three runs are in `docs/report/results/admission`
(`admission-1.json` to `admission-3.json`, medians in `medians.json`).
