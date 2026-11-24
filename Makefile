CC = gcc
CC_MAC = clang

# The largest task payload: 1,024 bytes by default. `make MAX_PAYLOAD=8192`
# builds a daemon for bigger tasks (a multiple of 16, at most 16,320, so that
# a 64-byte record plus the payload still fits a 16 KiB page).
MAX_PAYLOAD ?= 1024
DAEMON_OUT ?= quicxd

# the daemon goes to ./quicxd; tests, benchmarks and the 8 KiB daemon to build/
BUILD = build

# every header sits next to its .c file and is included by its component:
# #include "alloc/apool.h", so -Isrc is the only include path
CFLAGS_COMMON = -std=c17 \
         -D_GNU_SOURCE \
         -Isrc \
         -DPROTOCOL_MAX_PAYLOAD=$(MAX_PAYLOAD) \
         -DAPOOL_MAX_BLOCK=$(shell expr $(MAX_PAYLOAD) + 64)

CFLAGS_DEBUG = -g

CFLAGS_RELEASE = -O2

# PMAD, the 1.x allocator: only the allocator benchmark builds it, as the
# baseline apool is measured against
PMAD_SRC = bench/pmad/src/PMAD.c \
           bench/pmad/src/incPMAD.c \
           bench/pmad/src/MemoryPool.c \
           bench/pmad/src/BlockHeader.c
PMAD_INC = -Ibench/pmad/include -Ibench/pmad/include/structures

# the daemon without main.c, grouped by component (folders under src/)
QUICX_CORE = src/alloc/apool.c \
             src/qxp/protocol.c \
             src/net/event_loop.c src/net/server.c \
             src/core/quicx.c src/core/daemon.c src/core/queue.c \
             src/core/dispatcher.c src/core/workers.c src/core/admission.c \
             src/core/stats.c \
             src/config/config.c src/config/pool_config.c \
             src/cli/cli.c

# make          the daemon and its tests
# make test     run the unit tests
# make e2e      run the end-to-end tests against ./quicxd (python3)
# make bench    the allocator benchmarks
all: quicxd test_apool test_config test_wire test_admission

bench: bench_apool bench_purge

# the targets list no header or source dependencies, so they are phony:
# every `make` rebuilds them and a source change can never leave a stale binary
.PHONY: all bench test e2e check-big quicxd-8k release clean dsym_remove build_dir \
        quicxd test_apool test_config test_wire test_admission bench_apool bench_purge

build_dir:
	@mkdir -p $(BUILD)

quicxd:
	$(CC) $(CFLAGS_COMMON) $(CFLAGS_DEBUG) \
	src/main.c $(QUICX_CORE) \
	-o $(DAEMON_OUT) -lpthread

test_apool: build_dir
	$(CC) $(CFLAGS_COMMON) $(CFLAGS_DEBUG) \
	tests/unit/test_apool.c src/alloc/apool.c \
	-o $(BUILD)/$@

# allocator benchmarks (release flags): the ablation of every apool feature
# against PMAD, and purge's memory and time cost
bench_apool: build_dir
	$(CC) $(CFLAGS_COMMON) $(CFLAGS_RELEASE) \
	$(PMAD_INC) bench/bench_apool.c bench/pmad_layout.c src/alloc/apool.c $(PMAD_SRC) \
	-o $(BUILD)/$@

bench_purge: build_dir
	$(CC) $(CFLAGS_COMMON) $(CFLAGS_RELEASE) \
	bench/bench_purge.c src/alloc/apool.c \
	-o $(BUILD)/$@

test_config: build_dir
	$(CC) $(CFLAGS_COMMON) $(CFLAGS_DEBUG) \
	tests/unit/test_config.c src/config/config.c src/config/pool_config.c src/alloc/apool.c \
	-o $(BUILD)/$@

test_wire: build_dir
	$(CC) $(CFLAGS_COMMON) $(CFLAGS_DEBUG) \
	tests/unit/test_wire.c src/qxp/protocol.c \
	-o $(BUILD)/$@

test_admission: build_dir
	$(CC) $(CFLAGS_COMMON) $(CFLAGS_DEBUG) \
	tests/unit/test_admission.c $(QUICX_CORE) \
	-o $(BUILD)/$@ -lpthread

test: test_apool test_config test_wire test_admission
	@echo "--- test_apool ---"
	./$(BUILD)/test_apool
	@echo "--- test_config ---"
	./$(BUILD)/test_config quicx.conf
	@echo "--- test_wire ---"
	./$(BUILD)/test_wire
	@echo "--- test_admission ---"
	./$(BUILD)/test_admission
	@echo "--- all unit tests passed ---"

e2e: quicxd
	python3 tests/e2e/e2e_tests.py ./quicxd 16391

# a daemon for payloads up to 8 KiB, checked against the capacity formula
quicxd-8k: build_dir
	$(MAKE) --no-print-directory quicxd MAX_PAYLOAD=8192 DAEMON_OUT=$(BUILD)/quicxd-8k

check-big: quicxd-8k
	python3 tests/e2e/big_payload_check.py ./$(BUILD)/quicxd-8k 8192

# release binaries are stripped: a download carries no debug info
VERSION ?= dev

RELEASE_DIR = releases
OUT_DIR = $(RELEASE_DIR)/v$(VERSION)

release_dir:
	mkdir -p $(OUT_DIR)

linux_x86:
	zig cc $(CFLAGS_COMMON) $(CFLAGS_RELEASE) -s \
	src/main.c $(QUICX_CORE) \
	-target x86_64-linux-gnu \
	-o $(OUT_DIR)/quicx-linux-x86_64 -lpthread

linux_arm:
	zig cc $(CFLAGS_COMMON) $(CFLAGS_RELEASE) -s \
	src/main.c $(QUICX_CORE) \
	-target aarch64-linux-gnu \
	-o $(OUT_DIR)/quicx-linux-aarch64 -lpthread

mac_x86:
	zig cc $(CFLAGS_COMMON) $(CFLAGS_RELEASE) -s \
	src/main.c $(QUICX_CORE) \
	-target x86_64-macos-none \
	-o $(OUT_DIR)/quicx-darwin-x86_64 -lpthread

mac_arm:
	zig cc $(CFLAGS_COMMON) $(CFLAGS_RELEASE) -s \
	src/main.c $(QUICX_CORE) \
	-target aarch64-macos-none \
	-o $(OUT_DIR)/quicx-darwin-arm64 -lpthread

release: release_dir linux_x86 linux_arm mac_x86 mac_arm
	cd $(OUT_DIR) && { sha256sum quicx-* 2>/dev/null || shasum -a 256 quicx-*; } > SHA256SUMS
	@echo "✓ Release v$(VERSION) built in $(OUT_DIR), with SHA256SUMS"

clean:
	rm -f quicxd
	rm -rf $(BUILD) $(RELEASE_DIR)

dsym_remove:
	rm -rf *.dSYM $(BUILD)/*.dSYM
