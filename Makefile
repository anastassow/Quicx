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
             quicx/src/quicx.c

all: quicxd test_queue test_workers test_dispatcher \
     test_protocol test_wire test_server

quicxd:
	$(CC) $(CFLAGS_COMMON) $(CFLAGS_DEBUG) \
	quicx/src/main.c $(QUICX_CORE) $(PMAD_SRC) \
	-o $@ -lpthread

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

test: all
	@echo "--- running test_queue ---"
	./test_queue
	@echo "--- running test_workers ---"
	./test_workers
	@echo "--- running test_dispatcher ---"
	./test_dispatcher
	@echo "--- running test_protocol ---"
	./test_protocol
	@echo "--- running test_wire ---"
	./test_wire
	@echo "--- running test_server ---"
	./test_server
	@echo "--- all tests passed ---"

VERSION ?= dev

RELEASE_DIR = releases
OUT_DIR = $(RELEASE_DIR)/v$(VERSION)

release_dir:
	mkdir -p $(OUT_DIR)

linux_x86:
	zig cc $(CFLAGS_COMMON) $(CFLAGS_RELEASE) \
	quicx/src/main.c $(QUICX_CORE) $(PMAD_SRC) \
	-target x86_64-linux-gnu \
	-o $(OUT_DIR)/quicx-linux-x86_64 -lpthread

linux_arm:
	zig cc $(CFLAGS_COMMON) $(CFLAGS_RELEASE) \
	quicx/src/main.c $(QUICX_CORE) $(PMAD_SRC) \
	-target aarch64-linux-gnu \
	-o $(OUT_DIR)/quicx-linux-aarch64 -lpthread

mac_x86:
	zig cc $(CFLAGS_COMMON) $(CFLAGS_RELEASE) \
	quicx/src/main.c $(QUICX_CORE) $(PMAD_SRC) \
	-target x86_64-macos-none \
	-o $(OUT_DIR)/quicx-darwin-x86_64 -lpthread

mac_arm:
	zig cc $(CFLAGS_COMMON) $(CFLAGS_RELEASE) \
	quicx/src/main.c $(QUICX_CORE) $(PMAD_SRC) \
	-target aarch64-macos-none \
	-o $(OUT_DIR)/quicx-darwin-arm64 -lpthread

release: release_dir linux_x86 linux_arm mac_x86 mac_arm
	@echo "✓ Release v$(VERSION) built in $(OUT_DIR)"

clean:
	rm -f quicxd test_queue test_workers test_dispatcher \
	      test_protocol test_wire test_server test_daemon test_stats test_unix
	rm -rf $(RELEASE_DIR)

dsym_remove:
	rm -rf *.dSYM