#ifndef CLI_H
#define CLI_H

#include <stddef.h>
#include "stats.h"

#define VERSION          "2.1.0"
#define PID_FILE         "/tmp/quicx.pid"

/* unix socket the daemon listens on — cli connects here for stats requests */
#define UNIX_SOCKET_PATH "/tmp/quicx.sock"

/*
where the daemon on `port` keeps its CLI socket and pid file. The default
port keeps the paths above, unchanged since 1.x; any other port gets its
own pair (/tmp/quicx-<port>.sock and .pid), so several daemons, one per
port, can share a machine without taking over each other's socket.
*/
void cli_runtime_paths(int port, char* sock, size_t sock_len, char* pid, size_t pid_len);

/*
connects to the running quicx daemon over its CLI socket at sock_path
returns a connected fd on success, -1 on error (prints diagnostic)
*/
int cli_connect(const char* sock_path);

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
int  cmd_stop(int argc, char* argv[]);
int  cmd_status(int argc, char* argv[]);
int  cmd_version(void);

/* prints top-level usage */
void print_usage(void);

#endif
