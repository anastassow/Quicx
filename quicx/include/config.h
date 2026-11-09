#ifndef CONFIG_H
#define CONFIG_H

#include "pool_config.h"

#define DEFAULT_PORT 16381

/*
everything `quicx start` reads, in this order: the built-in defaults, then
the file given with --config, then the command line (--port wins over the
file). the file is ini-style:

    [server]      port
    [memory]      pool_size
    [allocator]   reserve, hardening, purge, fullest_first,
                  warm_pages, reserve_pages

[server] is kept here; [memory] and [allocator] describe the pools and are
a PoolConfig (pool_config.h). quicx.conf in the repository lists every key
with its default.
*/
typedef struct {
    int        port;  /* [server] */
    PoolConfig pool;  /* [memory] and [allocator] */
} Config;

/* fills c with the built-in defaults */
void config_defaults(Config* c);

/* checks c, printing an error and exiting the process if it cannot be used */
void config_validate(const Config* c);

/*
reads the ini-style file at path into c, over what c already holds.
an unknown section or key, or a bad value, prints the file and line and
exits the process
*/
void config_load(Config* c, const char* path);

#endif
