#include "quicx.h"
#include "server.h"

/* pages a size class needs to hold `count` blocks of `size` bytes */
#define PAGES_FOR(count, size) \
    (((count) + (APOOL_PAGE_SIZE / (size)) - 1) / (APOOL_PAGE_SIZE / (size)))

/* enough control pages for MAX_CONNECTIONS connections and as many workers */
#define CTRL_PAGES ((uint32_t)(PAGES_FOR(MAX_CONNECTIONS, sizeof(Connection)) + \
                               PAGES_FOR(MAX_CONNECTIONS, sizeof(Worker))))

ApoolStatus quicx_init(Quicx* quicx, const Config* cfg) {
    /* the budget is split, never exceeded: control pages first, tasks get the rest */
    size_t ctrl_bytes = apool_bytes_for_pages(CTRL_PAGES);
    if (cfg->pool_size <= ctrl_bytes + apool_bytes_for_pages(1))
        return APOOL_ERR_TOO_SMALL;

    ApoolStatus status = apool_init(&quicx->ctrl, ctrl_bytes);
    if (status != APOOL_OK) return status;

    status = apool_init(&quicx->mem, cfg->pool_size - ctrl_bytes);
    if (status != APOOL_OK) { apool_destroy(&quicx->ctrl); return status; }

    queue_init(&quicx->queue);
    workers_init(&quicx->pool);
    stats_init(&quicx->stats);

    return APOOL_OK;
}

void quicx_destroy(Quicx* quicx) {
    apool_destroy(&quicx->mem);
    apool_destroy(&quicx->ctrl);
}
