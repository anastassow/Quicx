CC = gcc
CC_MAC = clang

# The largest task payload: 1,024 bytes by default. `make MAX_PAYLOAD=8192`
# builds a daemon for bigger tasks (a multiple of 16, at most 16,320, so that
# a 64-byte record plus the payload still fits a 16 KiB page).
MAX_PAYLOAD ?= 1024
DAEMON_OUT ?= quicxd

CFLAGS_COMMON = -std=c17 \
         -D_GNU_SOURCE \
         -Iquicx/include \
         -DPROTOCOL_MAX_PAYLOAD=$(MAX_PAYLOAD) \
         -DAPOOL_MAX_BLOCK=$(shell expr $(MAX_PAYLOAD) + 64)

CFLAGS_DEBUG = -g

CFLAGS_RELEASE = -O2

# PMAD, the 1.x allocator: only the allocator benchmark builds it, as the
# baseline apool is measured against
PMAD_SRC = pmad/src/PMAD.c \
           pmad/src/incPMAD.c \
           pmad/src/MemoryPool.c \
           pmad/src/BlockHeader.c
PMAD_INC = -Ipmad/include -Ipmad/include/structures

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
             quicx/src/apool.c \
             quicx/src/admission.c

# make          the daemon and its tests
# make test     run the unit tests
# make e2e      run the end-to-end tests against ./quicxd (python3)
# make bench    the allocator benchmarks
all: quicxd test_apool test_config test_wire test_admission

bench: bench_apool bench_purge

# the targets list no header or source dependencies, so they are phony:
# every `make` rebuilds them and a source change can never leave a stale binary
.PHONY: all bench test e2e check-big quicxd-8k release clean dsym_remove \
        quicxd test_apool test_config test_wire test_admission bench_apool bench_purge

quicxd:
	$(CC) $(CFLAGS_COMMON) $(CFLAGS_DEBUG) \
	quicx/src/main.c $(QUICX_CORE) \
	-o $(DAEMON_OUT) -lpthread

test_apool:
	$(CC) $(CFLAGS_COMMON) $(CFLAGS_DEBUG) \
	quicx/src/tests/test_apool.c quicx/src/apool.c \
	-o $@

# allocator benchmarks (release flags): the ablation of every apool feature
# against PMAD, and purge's memory and time cost
bench_apool:
	$(CC) $(CFLAGS_COMMON) $(CFLAGS_RELEASE) \
	$(PMAD_INC) quicx/src/tests/bench_apool.c quicx/src/tests/pmad_layout.c quicx/src/apool.c $(PMAD_SRC) \
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

test_admission:
	$(CC) $(CFLAGS_COMMON) $(CFLAGS_DEBUG) \
	quicx/src/tests/test_admission.c $(QUICX_CORE) \
	-o $@ -lpthread

test: test_apool test_config test_wire test_admission
	@echo "--- test_apool ---"
	./test_apool
	@echo "--- test_config ---"
	./test_config quicx.conf
	@echo "--- test_wire ---"
	./test_wire
	@echo "--- test_admission ---"
	./test_admission
	@echo "--- all unit tests passed ---"

e2e: quicxd
	python3 docs/report/scripts/e2e_tests.py ./quicxd 16391

# a daemon for payloads up to 8 KiB, checked against the capacity formula
quicxd-8k:
	$(MAKE) --no-print-directory quicxd MAX_PAYLOAD=8192 DAEMON_OUT=quicxd-8k

check-big: quicxd-8k
	python3 docs/report/scripts/big_payload_check.py ./quicxd-8k 8192

# release binaries are stripped: a download carries no debug info
VERSION ?= dev

RELEASE_DIR = releases
OUT_DIR = $(RELEASE_DIR)/v$(VERSION)

release_dir:
	mkdir -p $(OUT_DIR)

linux_x86:
	zig cc $(CFLAGS_COMMON) $(CFLAGS_RELEASE) -s \
	quicx/src/main.c $(QUICX_CORE) \
	-target x86_64-linux-gnu \
	-o $(OUT_DIR)/quicx-linux-x86_64 -lpthread

linux_arm:
	zig cc $(CFLAGS_COMMON) $(CFLAGS_RELEASE) -s \
	quicx/src/main.c $(QUICX_CORE) \
	-target aarch64-linux-gnu \
	-o $(OUT_DIR)/quicx-linux-aarch64 -lpthread

mac_x86:
	zig cc $(CFLAGS_COMMON) $(CFLAGS_RELEASE) -s \
	quicx/src/main.c $(QUICX_CORE) \
	-target x86_64-macos-none \
	-o $(OUT_DIR)/quicx-darwin-x86_64 -lpthread

mac_arm:
	zig cc $(CFLAGS_COMMON) $(CFLAGS_RELEASE) -s \
	quicx/src/main.c $(QUICX_CORE) \
	-target aarch64-macos-none \
	-o $(OUT_DIR)/quicx-darwin-arm64 -lpthread

release: release_dir linux_x86 linux_arm mac_x86 mac_arm
	@echo "✓ Release v$(VERSION) built in $(OUT_DIR)"

clean:
	rm -f quicxd quicxd-8k test_apool test_config test_wire test_admission bench_apool bench_purge
	rm -rf $(RELEASE_DIR)

dsym_remove:
	rm -rf *.dSYM