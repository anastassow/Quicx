CC     = gcc
CFLAGS = -g -std=c17 \
         -Ipmad/include \
         -Ipmad/include/stuctures \
         -Iquicx/include

PMAD_SRC = pmad/src/PMAD.c \
           pmad/src/incPMAD.c \
           pmad/src/MemoryPool.c \
           pmad/src/BlockHeader.c \
           pmad/src/SizeClass.c

QUICX_CORE = quicx/src/queue.c \
             quicx/src/workers.c \
             quicx/src/dispacher.c \
             quicx/src/protocol.c \
             quicx/src/event_loop.c \
             quicx/src/server.c \
             quicx/src/daemon.c \
             quicx/src/stats.c

all: quicxd test_queue test_workers test_dispatcher \
     test_protocol test_server

quicxd: quicx/src/main.c $(QUICX_CORE) $(PMAD_SRC)
	$(CC) $(CFLAGS) $^ -o $@ -lpthread

test_daemon: quicx/src/test_daemon.c \
             $(QUICX_CORE) $(PMAD_SRC)
	$(CC) $(CFLAGS) $^ -o $@ -lpthread

test_queue: quicx/src/test_queue.c \
            quicx/src/queue.c $(PMAD_SRC)
	$(CC) $(CFLAGS) $^ -o $@

test_workers: quicx/src/test_workers.c \
              quicx/src/workers.c \
              quicx/src/queue.c $(PMAD_SRC)
	$(CC) $(CFLAGS) $^ -o $@

test_dispatcher: quicx/src/test_dispatcher.c \
                 quicx/src/queue.c \
                 quicx/src/workers.c \
                 quicx/src/dispacher.c \
                 $(PMAD_SRC)
	$(CC) $(CFLAGS) $^ -o $@

test_protocol: quicx/src/test_protocol.c \
               quicx/src/protocol.c
	$(CC) $(CFLAGS) $^ -o $@ -lpthread

test_server: quicx/src/test_server.c \
             $(QUICX_CORE) $(PMAD_SRC)
	$(CC) $(CFLAGS) $^ -o $@ -lpthread

test_stats: quicx/src/test_stats.c \
            $(QUICX_CORE) $(PMAD_SRC)
	$(CC) $(CFLAGS) $^ -o $@ -lpthread

test_unix: quicx/src/test_unix.c \
           $(QUICX_CORE) $(PMAD_SRC)
	$(CC) $(CFLAGS) $^ -o $@ -lpthread

test: all
	@echo "--- running test_queue ---"
	./test_queue
	@echo "--- running test_workers ---"
	./test_workers
	@echo "--- running test_dispatcher ---"
	./test_dispatcher
	@echo "--- running test_protocol ---"
	./test_protocol
	@echo "--- running test_server ---"
	./test_server
	@echo "--- all tests passed ---"

clean:
	rm -f quicxd test_queue test_workers test_dispatcher \
	      test_protocol test_server test_daemon test_stats test_unix