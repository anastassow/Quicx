#ifndef PMAD_LAYOUT_H
#define PMAD_LAYOUT_H

#include <stddef.h>

#include "incPMAD.h"

/*
the size-class layout quicx gave PMAD, its static allocator, up to v1.0.4.
the daemon no longer uses it — apool sizes itself — but bench_apool still
measures PMAD with it as the baseline, so it lives here with the benchmark
*/
typedef struct {
    size_t sizes[MAX_PMAD_CLASSES];
    size_t percentages[MAX_PMAD_CLASSES];
    int    class_count;
} PmadLayout;

/* fills l with the v1.0.4 default layout: every 16 B class up to 1,024 B */
void pmad_layout_default(PmadLayout* l);

#endif
