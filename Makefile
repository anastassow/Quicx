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

QUICX_SRC = quicx/src/queue.c \
            quicx/src/workers.c \
            quicx/src/dispacher.c

all: test_queue test_workers test_dispatcher test_protocol

test_queue: quicx/src/test_queue.c quicx/src/queue.c $(PMAD_SRC)
	$(CC) $(CFLAGS) $^ -o $@

test_workers: quicx/src/test_workers.c quicx/src/workers.c \
              quicx/src/queue.c $(PMAD_SRC)
	$(CC) $(CFLAGS) $^ -o $@

test_dispatcher: quicx/src/test_dispatcher.c $(QUICX_SRC) $(PMAD_SRC)
	$(CC) $(CFLAGS) $^ -o $@

test_protocol: quicx/src/test_protocol.c quicx/src/protocol.c
	$(CC) $(CFLAGS) $^ -o $@

test: all
	@echo "--- running test_queue ---"
	./test_queue
	@echo "--- running test_workers ---"
	./test_workers
	@echo "--- running test_dispatcher ---"
	./test_dispatcher
	@echo "--- running test_protocol ---"
	./test_protocol
	@echo "--- all tests passed ---"

clean:
	rm -f test_queue test_workers test_dispatcher test_protocol