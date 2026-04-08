#ifndef STATS_H
#define STATS_H

#include <stdint.h>
#include <time.h>
#include <arpa/inet.h>

// internal counters — host byte order — lives in stats.c as static global
typedef struct {
    uint64_t tasks_submitted;
    uint64_t tasks_completed;
    uint64_t tasks_failed;
    time_t   start_time;
} Stats;


// binary wire format — sent to CLI via MSG_STATS_RESPONSE
// all fields network byte order
// followed by class_count * StatsClass entries
typedef struct __attribute__((packed)) {
    uint32_t idle_workers;
    uint32_t busy_workers;
    uint32_t queued_tasks;
    uint64_t tasks_submitted;
    uint64_t tasks_completed;
    uint64_t tasks_failed;
    uint64_t uptime_seconds;
    uint32_t class_count;
} StatsHeader;

typedef struct __attribute__((packed)) {
    uint32_t block_size;
    uint32_t total_blocks;
    uint32_t allocated_blocks;
} StatsClass;

void   stats_init(void);
void   stats_task_submitted(void);
void   stats_task_completed(void);
void   stats_task_failed(void);
Stats* stats_get(void);

#endif