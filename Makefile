CC     = gcc
CFLAGS = -g -std=c17 -Ipmad/include -Ipmad/include/stuctures -Iquicx/include

PMAD_SRC = pmad/src/PMAD.c \
           pmad/src/incPMAD.c \
           pmad/src/MemoryPool.c \
           pmad/src/BlockHeader.c \
           pmad/src/SizeClass.c

all: test_queue test_workers

test_queue: quicx/src/test_queue.c quicx/src/queue.c $(PMAD_SRC)
	$(CC) $(CFLAGS) $^ -o $@

test_workers: quicx/src/test_workers.c quicx/src/workers.c $(PMAD_SRC)
	$(CC) $(CFLAGS) $^ -o $@

test: test_queue test_workers
	@echo "--- running test_queue ---"
	./test_queue
	@echo "--- running test_workers ---"
	./test_workers
	@echo "--- all tests passed ---"

clean:
	rm -f test_queue test_workers