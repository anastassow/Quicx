#include "../include/incPMAD.h"
#include <stdlib.h>
#include <stdio.h>

PmadStatus pmad_init(PMAD* pmad, const size_t* class_sizes, uint8_t num_size_classes,
                     const size_t* percentages, size_t pool_size) {
    pmad->pool_head = NULL;

    void* pool_mem = get_memory_pool_from_os(pool_size);
    if (!pool_mem)
        return PMAD_ERR_MAP_FAILED;

    attach_new_pool(pmad, pool_mem, pool_size);

    if (PMAD_init(pmad, class_sizes, num_size_classes, pool_size) != PMAD_OK)
        return PMAD_ERR_INIT_FAILED;

    if (split_pool_by_percentage(pmad, percentages, num_size_classes) != PMAD_OK)
        return PMAD_ERR_INCOMPLETE_PERCENTAGE;

    return PMAD_OK;
}

void* pmad_alloc(PMAD* pmad, size_t size) {
    void* memory = NULL;
    PmadStatus status = PMAD_alloc(pmad, size, &memory);
    if (status != PMAD_OK) {
        fprintf(stderr, "pmad_alloc failed: %d\n", status);
        return NULL;
    }

    return memory;
}

PmadStatus pmad_free(PMAD* pmad, void* ptr) {
    return PMAD_free(pmad, ptr);
}

PmadStatus pmad_destroy(PMAD* pmad) {
    MemoryPool* pool = pmad->pool_head;
    while (pool) {
        MemoryPool* next = pool->next;
        PmadStatus status = free_memory_pool(pool, pmad->pool_size);
        if (status != PMAD_OK)
            return status;
        pool = next;
    }
    pmad->size_classes     = NULL;
    pmad->pool_head        = NULL;
    pmad->num_size_classes = 0;

    return PMAD_OK;
}

int pmad_get_stats(PMAD* pmad, PmadClassStats* out, int max_classes) {
    int count = 0;
    uint8_t n = pmad->num_size_classes;
    int limit = (max_classes < (int)n) ? max_classes : (int)n;
    for (int i = 0; i < limit; i++) {
        out[i].block_size       = (uint32_t)pmad->size_classes[i].block_size;
        out[i].total_blocks     = (uint32_t)pmad->size_classes[i].total_blocks;
        out[i].allocated_blocks = (uint32_t)pmad->size_classes[i].allocated_blocks;
        count++;
    }
    return count;
}
