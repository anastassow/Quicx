CC = gcc
CC_MAC = clang

CFLAGS_COMMON = -std=c17 \
         -D_GNU_SOURCE \
         -Ipmad/include \
         -Ipmad/include/stuctures \
         -Iquicx/include

CFLAGS_DEBUG = -g

CFLAGS_RELEASE = -O2

PMAD_SRC = pmad/src/PMAD.c \
           pmad/src/incPMAD.c \
           pmad/src/MemoryPool.c \
           pmad/src/BlockHeader.c

QUICX_CORE = quicx/src/queue.c \
             quicx/src/workers.c \
             quicx/src/dispacher.c \
             quicx/src/protocol.c \
             quicx/src/event_loop.c \
             quicx/src/server.c \
             quicx/src/daemon.c \
             quicx/src/stats.c \
             quicx/src/cli.c \
             quicx/src/config.c \
             quicx/src/pool_config.c \
             quicx/src/quicx.c \
             quicx/src/apool.c

# make          the daemon and its tests
# make test     run the unit tests
# make e2e      run the end-to-end tests against ./quicxd (python3)
# make bench    the allocator benchmarks
all: quicxd test_apool test_config test_wire

bench: bench_apool bench_purge

.PHONY: all bench test e2e legacy_tests release clean dsym_remove

quicxd:
	$(CC) $(CFLAGS_COMMON) $(CFLAGS_DEBUG) \
	quicx/src/main.c $(QUICX_CORE) \
	-o $@ -lpthread

# ---- legacy tests ---------------------------------------------------------
# written for PMAD, the static allocator of v1.x; they do not build against
# apool yet and are not part of `make` or `make test`. the daemon itself is
# covered end to end by `make e2e` (27 checks).
legacy_tests: test_queue test_workers test_dispatcher test_protocol test_server

test_daemon:
	$(CC) $(CFLAGS_COMMON) $(CFLAGS_DEBUG) \
	quicx/src/tests/test_daemon.c $(QUICX_CORE) $(PMAD_SRC) \
	-o $@ -lpthread

test_queue:
	$(CC) $(CFLAGS_COMMON) $(CFLAGS_DEBUG) \
	quicx/src/tests/test_queue.c quicx/src/queue.c $(PMAD_SRC) \
	-o $@

test_workers:
	$(CC) $(CFLAGS_COMMON) $(CFLAGS_DEBUG) \
	quicx/src/tests/test_workers.c quicx/src/workers.c quicx/src/queue.c $(PMAD_SRC) \
	-o $@

test_dispatcher:
	$(CC) $(CFLAGS_COMMON) $(CFLAGS_DEBUG) \
	quicx/src/tests/test_dispatcher.c quicx/src/queue.c quicx/src/workers.c quicx/src/dispacher.c quicx/src/protocol.c $(PMAD_SRC) \
	-o $@

test_protocol:
	$(CC) $(CFLAGS_COMMON) $(CFLAGS_DEBUG) \
	quicx/src/tests/test_protocol.c quicx/src/protocol.c \
	-o $@ -lpthread

test_apool:
	$(CC) $(CFLAGS_COMMON) $(CFLAGS_DEBUG) \
	quicx/src/tests/test_apool.c quicx/src/apool.c \
	-o $@

# allocator benchmarks (release flags): the ablation of every apool feature
# against PMAD, and purge's memory and time cost
bench_apool:
	$(CC) $(CFLAGS_COMMON) $(CFLAGS_RELEASE) \
	quicx/src/tests/bench_apool.c quicx/src/tests/pmad_layout.c quicx/src/apool.c $(PMAD_SRC) \
	-o $@

bench_purge:
	$(CC) $(CFLAGS_COMMON) $(CFLAGS_RELEASE) \
	quicx/src/tests/bench_purge.c quicx/src/apool.c \
	-o $@

test_config:
	$(CC) $(CFLAGS_COMMON) $(CFLAGS_DEBUG) \
	quicx/src/tests/test_config.c quicx/src/config.c quicx/src/pool_config.c quicx/src/apool.c \
	-o $@

test_wire:
	$(CC) $(CFLAGS_COMMON) $(CFLAGS_DEBUG) \
	quicx/src/tests/test_wire.c quicx/src/protocol.c \
	-o $@

test_server:
	$(CC) $(CFLAGS_COMMON) $(CFLAGS_DEBUG) \
	quicx/src/tests/test_server.c $(QUICX_CORE) $(PMAD_SRC) \
	-o $@ -lpthread

test_stats:
	$(CC) $(CFLAGS_COMMON) $(CFLAGS_DEBUG) \
	quicx/src/tests/test_stats.c $(QUICX_CORE) $(PMAD_SRC) \
	-o $@ -lpthread

test_unix:
	$(CC) $(CFLAGS_COMMON) $(CFLAGS_DEBUG) \
	quicx/src/tests/test_unix.c $(QUICX_CORE) $(PMAD_SRC) \
	-o $@ -lpthread

test: test_apool test_config test_wire
	@echo "--- test_apool ---"
	./test_apool
	@echo "--- test_config ---"
	./test_config quicx.conf
	@echo "--- test_wire ---"
	./test_wire
	@echo "--- all unit tests passed ---"

e2e: quicxd
	python3 docs/report/scripts/e2e_tests.py ./quicxd 16391

VERSION ?= dev

RELEASE_DIR = releases
OUT_DIR = $(RELEASE_DIR)/v$(VERSION)

release_dir:
	mkdir -p $(OUT_DIR)

linux_x86:
	zig cc $(CFLAGS_COMMON) $(CFLAGS_RELEASE) \
	quicx/src/main.c $(QUICX_CORE) \
	-target x86_64-linux-gnu \
	-o $(OUT_DIR)/quicx-linux-x86_64 -lpthread

linux_arm:
	zig cc $(CFLAGS_COMMON) $(CFLAGS_RELEASE) \
	quicx/src/main.c $(QUICX_CORE) \
	-target aarch64-linux-gnu \
	-o $(OUT_DIR)/quicx-linux-aarch64 -lpthread

mac_x86:
	zig cc $(CFLAGS_COMMON) $(CFLAGS_RELEASE) \
	quicx/src/main.c $(QUICX_CORE) \
	-target x86_64-macos-none \
	-o $(OUT_DIR)/quicx-darwin-x86_64 -lpthread

mac_arm:
	zig cc $(CFLAGS_COMMON) $(CFLAGS_RELEASE) \
	quicx/src/main.c $(QUICX_CORE) \
	-target aarch64-macos-none \
	-o $(OUT_DIR)/quicx-darwin-arm64 -lpthread

release: release_dir linux_x86 linux_arm mac_x86 mac_arm
	@echo "✓ Release v$(VERSION) built in $(OUT_DIR)"

clean:
	rm -f quicxd test_queue test_workers test_dispatcher \
	      test_protocol test_wire test_apool test_config test_server test_daemon test_stats test_unix \
	      bench_apool bench_purge
	rm -rf $(RELEASE_DIR)

dsym_remove:
	rm -rf *.dSYM