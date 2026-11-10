# PMAD

The fixed-layout allocator Quicx used up to 1.0.4. The daemon has not used it
since 2.0, which replaced it with apool (`quicx/src/apool.c`).

It stays here for one reason: `make bench` builds `bench_apool`, which measures
apool against PMAD's default layout. That comparison is where the capacity
numbers in the README come from (655 tasks of 100 B in 8 MiB with PMAD,
46,593 with apool).
