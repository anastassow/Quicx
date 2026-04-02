CC = gcc
CFLAGS = -g -Ipmad/include -Iquicx/include

all: test

test:
	$(CC) $(CFLAGS) quicx/src/test_queue.c quicx/src/queue.c pmad/src/PMAD.c \
	pmad/src/incPMAD.c pmad/src/MemoryPool.c pmad/src/BlockHeader.c \
	pmad/src/SizeClass.c -o test_queue
	./test_queue

clean:
	rm -f test_queue
