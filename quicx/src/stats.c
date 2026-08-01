#include "stats.h"
#include <string.h>

void stats_init(Stats* s) {
    memset(s, 0, sizeof(*s));
    s->start_time = time(NULL);
}

void stats_task_submitted(Stats* s) { s->tasks_submitted++; }
void stats_task_completed(Stats* s) { s->tasks_completed++; }
void stats_task_failed(Stats* s)    { s->tasks_failed++;    }
