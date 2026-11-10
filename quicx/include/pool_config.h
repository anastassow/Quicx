#ifndef POOL_CONFIG_H
#define POOL_CONFIG_H

#include <stddef.h>
#include <stdint.h>

#include "apool.h"

#define DEFAULT_POOL_SIZE (8 * 1024 * 1024)

/* warm_pages / reserve_pages: let the pool size it from its page count */
#define POOL_AUTO 0

/*
how the daemon's memory is set up — the [memory] and [allocator] sections
of quicx.conf.

pool_size is the whole budget. quicx_init splits it once at start-up into
the control pool (connections and workers) and the task pool; it is never
exceeded. flags switch apool's features (APOOL_F_*, see apool.h) on or off
for both pools. warm_pages and reserve_pages only apply to the task pool:
the control pool is a few pages and keeps its own automatic values.
*/
typedef struct {
    size_t   pool_size;      /* bytes: control pool + task pool */
    uint32_t flags;          /* APOOL_F_* */
    uint32_t warm_pages;     /* empty pages kept backed before purging; POOL_AUTO = 1/8 of the pages */
    uint32_t reserve_pages;  /* free pages only for sizes that own none; POOL_AUTO = 1/64 of the pages */
    uint8_t  admission;      /* AdmissionMode: what a full budget does with a new task (admission.h) */
} PoolConfig;

/* what pool_config_set made of a key */
typedef enum {
    POOL_KEY_OK,       /* the key was a pool key and its value is stored */
    POOL_KEY_UNKNOWN,  /* not a pool key; nothing changed */
    POOL_KEY_BAD,      /* a pool key with a value that cannot be used; err says why */
} PoolKeyResult;

/* which of the daemon's two pools pool_config_apply sets up */
typedef enum {
    POOL_CONTROL,
    POOL_TASKS,
} PoolRole;

/* the built-in defaults: 8 MiB, APOOL_F_DEFAULT, automatic page counts */
void pool_config_defaults(PoolConfig* pc);

/*
stores one key = value from the config file. keys:
  pool_size      bytes, or with a K, M or G suffix (1024-based): 8M, 64MiB
  reserve        on / off
  hardening      on / off
  purge          on / off
  fullest_first  on / off
  warm_pages     auto, or a page count
  reserve_pages  auto, or a page count
  admission      refuse, oldest or value (see admission.h)
on POOL_KEY_BAD a one-line reason is written to err
*/
PoolKeyResult pool_config_set(PoolConfig* pc, const char* key, const char* value,
                              char* err, size_t errlen);

/* 0 if pc can be used, else -1 with a one-line reason in err */
int pool_config_check(const PoolConfig* pc, char* err, size_t errlen);

/*
applies pc to a pool that apool_init just set up, before its first
allocation. returns -1 if a page count is larger than the pool allows
*/
int pool_config_apply(const PoolConfig* pc, Apool* pool, PoolRole role);

/* the switched-on features as text, e.g. "reserve, hardening, purge" */
void pool_config_describe(const PoolConfig* pc, char* out, size_t n);

#endif
