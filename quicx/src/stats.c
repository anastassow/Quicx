#include "stats.h"
#include <string.h>

static Stats g_stats;

void stats_init(void) {
    memset(&g_stats, 0, sizeof(g_stats));
    g_stats.start_time = time(NULL);
}

void   stats_task_submitted(void) { g_stats.tasks_submitted++; }
void   stats_task_completed(void) { g_stats.tasks_completed++; }
void   stats_task_failed(void)    { g_stats.tasks_failed++;    }
Stats* stats_get(void)            { return &g_stats;           }