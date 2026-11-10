#ifndef QUICX_H
#define QUICX_H

#include "config.h"
#include "queue.h"
#include "workers.h"
#include "stats.h"
#include "apool.h"

/*
one shard's worth of live state: an allocator, a task queue, a worker
pool and its own counters. today the daemon runs a single Quicx; later
each thread gets its own, with something upstream deciding which shard
a worker/producer connects to.

the Config is deliberately not kept here — it is read once at startup
to size the allocator and never looked at again, and when there are
several shards there is still only one config for the whole daemon.

memory is two adaptive pools carved from the one configured budget
(see apool.h):

- ctrl holds Connections and Workers. it is sized for MAX_CONNECTIONS of
  each, so a queue that is full of tasks can still accept the workers
  that will drain it. with one shared pool, tasks could take every page
  and no worker could ever register again.
- mem holds Tasks and payloads: everything else in the budget.
*/
typedef struct Quicx {
    Apool       ctrl;
    Apool       mem;
    Queue       queue;
    WorkerPool  pool;
    Stats       stats;
    uint8_t     admission;   /* AdmissionMode, from [allocator] admission */
} Quicx;

/* initializes mem/queue/pool/stats; cfg is only read, never retained */
ApoolStatus quicx_init(Quicx* quicx, const Config* cfg);

/* unmaps the pool; queue/pool/stats hold no resources of their own */
void quicx_destroy(Quicx* quicx);

#endif
