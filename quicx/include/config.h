#ifndef CONFIG_H
#define CONFIG_H

#include <stddef.h>

#include "incPMAD.h"

#define DEFAULT_PORT 16381

/*
the built-in defaults carve every size class from ALIGNMENT up to
MAX_SIZE_OF_SIZE_CLASS, so the pool has to be big enough that even a
1% slice still holds a usable number of blocks of the largest classes
*/
#define DEFAULT_POOL_SIZE (8 * 1024 * 1024)

typedef struct {
    int    port;
    size_t pool_size;
    size_t sizes[MAX_PMAD_CLASSES];
    size_t percentages[MAX_PMAD_CLASSES];
    int    class_count;
} Config;

/* fills c with the built-in defaults */
void config_defaults(Config* c);

/* validates c, printing an error and exiting the process on failure */
void config_validate(Config* c);

/* 
loads config from an ini-style file at path
into c exits the process on error 
*/
void config_load(Config* c, const char* path);

#endif
