#ifndef STATS_H
#define STATS_H

#include <stdint.h>
#include <time.h>
#include <arpa/inet.h>

/* internal counters — host byte order — lives in stats.c as static global */
typedef struct {
    uint64_t tasks_submitted;
    uint64_t tasks_completed;
    uint64_t tasks_failed;
    time_t   start_time;
} Stats;

/*
binary wire format — sent to CLI via MSG_STATS_RESPONSE
all fields network byte order
followed by class_count * StatsClass entries
*/
typedef struct __attribute__((packed)) {
    uint32_t idle_workers;      /* idle workers count */
    uint32_t busy_workers;      /* busy workers count */
    uint32_t queued_tasks;      /* qued tasks count*/
    uint64_t tasks_submitted;   /* submitte tasks count */
    uint64_t tasks_completed;   /* completed tasks count */
    uint64_t tasks_failed;      /* failed tasks count */
    uint64_t uptime_seconds;    /* uptime in seconds */
    uint8_t  class_count;       /* classes count in PMAD */
} StatsHeader;

/* structure for information from PMAD */
typedef struct __attribute__((packed)) {
    uint32_t block_size;        /* block size*/
    uint32_t total_blocks;      /* total blocks */
    uint32_t allocated_blocks;  /* allocated blocks */
} StatsClass;

/* function for initializing the Stats struct*/
void   stats_init(void);

/* function for incrementing submitted tasks */
void   stats_task_submitted(void);

/* function for completed submitted tasks */
void   stats_task_completed(void);

/* function for incrementing failed tasks */
void   stats_task_failed(void);

/* getter for stats information */
Stats* stats_get(void);

#endif