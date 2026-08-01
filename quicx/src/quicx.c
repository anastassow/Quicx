#include "quicx.h"

PmadStatus quicx_init(Quicx* quicx, const Config* cfg) {
    PmadStatus status = pmad_init(&quicx->pmad, cfg->sizes, (uint8_t)cfg->class_count,
                                   cfg->percentages, cfg->pool_size);
    if (status != PMAD_OK) return status;

    queue_init(&quicx->queue);
    workers_init(&quicx->pool);
    stats_init(&quicx->stats);

    return PMAD_OK;
}

PmadStatus quicx_destroy(Quicx* quicx) {
    return pmad_destroy(&quicx->pmad);
}
