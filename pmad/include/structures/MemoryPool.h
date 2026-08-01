#ifndef MEMORY_POOL_H
#define MEMORY_POOL_H

#include <stdio.h>

/*
Structure for a pool of memory, used by the allocator.
*/
typedef struct MemoryPool {
    void* start;                /* pointer to the star of the memory pool */
    size_t size;                /* size of the memory pool */

    /*
    bytes of this pool committed: the fixed metadata (this header, the
    size class array, every BlockHeader) plus block_size for each live
    allocation. alloc and free must move it by the same amount or it
    drifts — see PMAD_alloc.

    all accounting targets pool_head, so this is only correct while PMAD
    holds a single pool. attaching a second one needs per pool attribution
    on both the alloc and the free side.
    */
    size_t used;

    struct MemoryPool* next;    /* pointer to the next pool (if there is one) */
} MemoryPool;

_Static_assert(sizeof(struct MemoryPool) == 32, "Size of MemoryPool is not 32");

#endif