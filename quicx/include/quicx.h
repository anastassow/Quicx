#ifndef QUICX_H
#define QUICX_H

#include "config.h"
#include "queue.h"
#include "workers.h"
#include "stats.h"
#include "incPMAD.h"

/*
one shard's worth of live state: an allocator, a task queue, a worker
pool and its own counters. today the daemon runs a single Quicx; later
each thread gets its own, with something upstream deciding which shard
a worker/producer connects to.

the Config is deliberately not kept here — it is read once at startup
to size the allocator and never looked at again, and when there are
several shards there is still only one config for the whole daemon.
*/
typedef struct {
    PMAD        pmad;
    Queue       queue;
    WorkerPool  pool;
    Stats       stats;
} Quicx;

/* initializes pmad/queue/pool/stats; cfg is only read, never retained */
PmadStatus quicx_init(Quicx* quicx, const Config* cfg);

/* tears down the allocator; queue/pool/stats hold no resources of their own */
PmadStatus quicx_destroy(Quicx* quicx);

#endif
