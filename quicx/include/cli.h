#ifndef CLI_H
#define CLI_H

#include "stats.h"

#define VERSION          "1.0.2"
#define PID_FILE         "/tmp/quicx.pid"

/* unix socket the daemon listens on — cli connects here for stats requests */
#define UNIX_SOCKET_PATH "/tmp/quicx.sock"

/*
connects to the running quicx daemon over UNIX_SOCKET_PATH
returns a connected fd on success, -1 on error (prints diagnostic)
*/
int cli_connect(void);

/* cli_read_stats: daemon replied in a format this build cannot parse */
#define CLI_STATS_VERSION_MISMATCH (-2) 

/*
reads a MSG_STATS_RESPONSE from fd into hdr/classes (max entries)
returns 1 on success, CLI_STATS_VERSION_MISMATCH on a protocol/layout
mismatch, -1 on any other error; class count is written to *count
*/
int cli_read_stats(int fd, StatsHeader* hdr,
                    StatsClass* classes, int max,
                    int* count);

/* pretty-prints a stats snapshot to stdout */
void cli_print_stats(const char* version, int pid, StatsHeader* hdr,
                      StatsClass* classes, int n);

/* command handlers, one per quicx subcommand */
int  cmd_start(int argc, char* argv[]);
int  cmd_stop(void);
int  cmd_status(void);
int  cmd_version(void);

/* prints top-level usage */
void print_usage(void);

#endif
