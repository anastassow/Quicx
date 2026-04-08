#ifndef INCPMAD_H
#define INCPMAD_H

#include "PMAD.h"

typedef struct {
    uint32_t block_size;
    uint32_t total_blocks;
    uint32_t allocated_blocks;
} PmadClassStats;

PmadStatus pmad_init(size_t* class_sizes, size_t* percentages);
void* pmad_alloc(size_t size);
PmadStatus pmad_free(void* ptr);
void pmad_destroy();

int pmad_get_stats(PmadClassStats* out, int max_classes);

#endif
