CC      ?= cc
CFLAGS   = -std=c17 -O2 -Wall -Wextra -Wno-unused-parameter -D_GNU_SOURCE
LDFLAGS  =

BINS = bench_worker bench_producer bench_report

all: $(BINS)

bench_worker: bench_worker.c bench_common.h workload.h
	$(CC) $(CFLAGS) $< -o $@ $(LDFLAGS)

bench_producer: bench_producer.c bench_common.h workload.h
	$(CC) $(CFLAGS) $< -o $@ $(LDFLAGS)

bench_report: bench_report.c bench_common.h
	$(CC) $(CFLAGS) $< -o $@ $(LDFLAGS)

clean:
	rm -f $(BINS)
	rm -rf results

.PHONY: all clean
