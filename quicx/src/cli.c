#include "platform.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/stat.h>
#include <arpa/inet.h>

#include "cli.h"
#include "protocol.h"
#include "config.h"
#include "queue.h"
#include "workers.h"
#include "stats.h"
#include "server.h"
#include "apool.h"

int cli_connect(void) {
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) { perror("socket"); return -1; }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, UNIX_SOCKET_PATH,
            sizeof(addr.sun_path) - 1);

    if (connect(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        fprintf(stderr,
                "error: cannot connect to quicx daemon\n"
                "       is it running? start with: quicx start\n");
        close(fd);
        return -1;
    }
    return fd;
}

int cli_read_stats(int fd, StatsHeader* hdr,
                    StatsClass* classes, int max,
                    int* count) {
    ProtocolHeader phdr;
    if (protocol_read_exact(fd, &phdr, PROTOCOL_HEADER_SIZE) <= 0)
        return -1;

    if (phdr.version != PROTOCOL_VERSION)
        return CLI_STATS_VERSION_MISMATCH;

    if ((MessageType)phdr.type != MSG_STATS_RESPONSE)
        return -1;

    uint32_t body_len = ntohl(phdr.length);
    if (body_len < sizeof(StatsHeader))
        return CLI_STATS_VERSION_MISMATCH;

    if (protocol_read_exact(fd, hdr, sizeof(StatsHeader)) <= 0)
        return -1;

    uint32_t n = hdr->class_count;
    if ((int)n > max) return -1;

    if (body_len != sizeof(StatsHeader) + n * sizeof(StatsClass))
        return CLI_STATS_VERSION_MISMATCH;

    if (n > 0 && protocol_read_exact(fd, classes,
                                     (int)(n * sizeof(StatsClass))) <= 0)
        return -1;

    *count = (int)n;
    return 1;
}

#define BAR_WIDTH 20

/* renders "1.00 MiB" / "999.9 KiB" / "512 B" into buf */
static void fmt_bytes(char* buf, size_t cap, uint64_t bytes) {
    if (bytes >= 1024ULL * 1024ULL)
        snprintf(buf, cap, "%.2f MiB", (double)bytes / (1024.0 * 1024.0));
    else if (bytes >= 1024ULL)
        snprintf(buf, cap, "%.1f KiB", (double)bytes / 1024.0);
    else
        snprintf(buf, cap, "%llu B", (unsigned long long)bytes);
}

/* renders 12847 as "12,847" into buf */
static void fmt_count(char* buf, size_t cap, uint64_t v) {
    char digits[32];
    int  len = snprintf(digits, sizeof(digits), "%llu", (unsigned long long)v);
    int  out = 0;

    for (int i = 0; i < len && out < (int)cap - 1; i++) {
        if (i > 0 && (len - i) % 3 == 0) buf[out++] = ',';
        buf[out++] = digits[i];
    }
    buf[out] = '\0';
}

/* fraction as a filled/empty block bar; den 0 renders empty */
static void print_bar(uint64_t num, uint64_t den) {
    int fill = den > 0 ? (int)((num * BAR_WIDTH) / den) : 0;
    if (fill > BAR_WIDTH) fill = BAR_WIDTH;

    for (int i = 0; i < BAR_WIDTH; i++)
        printf("%s", i < fill ? "█" : "░");
}

static double pct_of(uint64_t num, uint64_t den) {
    return den > 0 ? (double)num * 100.0 / (double)den : 0.0;
}

void cli_print_stats(const char* version, int pid, StatsHeader* hdr,
                      StatsClass* classes, int n) {
    uint32_t idle       = ntohl(hdr->idle_workers);
    uint32_t busy       = ntohl(hdr->busy_workers);
    uint32_t queued     = ntohl(hdr->queued_tasks);
    uint32_t registered = ntohl(hdr->workers_registered);
    uint64_t submitted  = ntohll(hdr->tasks_submitted);
    uint64_t completed  = ntohll(hdr->tasks_completed);
    uint64_t failed     = ntohll(hdr->tasks_failed);
    uint64_t uptime     = ntohll(hdr->uptime_seconds);
    uint64_t pool_size  = ntohll(hdr->pool_size);
    uint64_t usable     = ntohll(hdr->usable_bytes);
    uint64_t used       = ntohll(hdr->used_bytes);

    /*
    a task is in flight from the MSG_SUBMIT ack until a worker reports it
    done or failed, so it is either queued or held by a busy worker. signed
    because the counters can disagree if a task went missing.
    */
    int64_t in_flight = (int64_t)submitted - (int64_t)completed - (int64_t)failed;

    char nbuf[32], b1[32], b2[32];

    printf("\n  quicx v%s  ·  pid %d  ·  up %lluh %llum %02llus\n\n",
           version, pid,
           (unsigned long long)(uptime / 3600),
           (unsigned long long)((uptime % 3600) / 60),
           (unsigned long long)(uptime % 60));

    /* tasks */
    fmt_count(nbuf, sizeof(nbuf), submitted);
    printf("  tasks     submitted  %10s\n", nbuf);

    fmt_count(nbuf, sizeof(nbuf), completed);
    printf("            completed  %10s   ", nbuf);
    print_bar(completed, submitted);
    printf("   %5.1f%%\n", pct_of(completed, submitted));

    fmt_count(nbuf, sizeof(nbuf), failed);
    printf("            failed     %10s%*s%5.1f%%\n",
           nbuf, BAR_WIDTH + 6, "", pct_of(failed, submitted));

    fmt_count(nbuf, sizeof(nbuf), in_flight < 0 ? 0 : (uint64_t)in_flight);
    printf("            in flight  %10s   = %u queued + %u running\n",
           nbuf, queued, busy);

    fmt_count(nbuf, sizeof(nbuf), queued);
    printf("            queued     %10s\n\n", nbuf);

    /* workers */
    fmt_count(nbuf, sizeof(nbuf), busy);
    printf("  workers   busy       %10s   ", nbuf);
    print_bar(busy, (uint64_t)idle + busy);
    printf("   %5.1f%%\n", pct_of(busy, (uint64_t)idle + busy));

    fmt_count(nbuf, sizeof(nbuf), idle);
    printf("            idle       %10s\n", nbuf);

    fmt_count(nbuf, sizeof(nbuf), (uint64_t)idle + busy);
    printf("            total      %10s   · %u registered since start\n\n",
           nbuf, registered);

    /*
    memory — the pool is a fixed set of pages; "used" is the pages that are
    given to some block size right now, the rest are free for any size
    */
    uint64_t total_pages = usable / APOOL_PAGE_SIZE;
    uint64_t owned_pages = used / APOOL_PAGE_SIZE;

    char pair[48];
    fmt_count(b1, sizeof(b1), owned_pages);
    fmt_count(b2, sizeof(b2), total_pages);
    snprintf(pair, sizeof(pair), "%s / %s pages", b1, b2);
    printf("  memory    %-21s   ", pair);
    print_bar(owned_pages, total_pages);
    printf("   %5.1f%%\n", pct_of(owned_pages, total_pages));

    fmt_bytes(b1, sizeof(b1), pool_size);
    printf("            pool %s, %u KiB pages · %d size%s in use\n\n",
           b1, (unsigned)(APOOL_PAGE_SIZE / 1024), n, n == 1 ? "" : "s");

    for (int i = 0; i < n; i++) {
        uint32_t bs  = ntohl(classes[i].block_size);
        uint32_t tot = ntohl(classes[i].total_blocks);
        uint32_t use = ntohl(classes[i].allocated_blocks);
        uint32_t pages = tot / (uint32_t)(APOOL_PAGE_SIZE / bs);

        fmt_count(b1, sizeof(b1), use);
        fmt_count(b2, sizeof(b2), tot);
        printf("         %5uB  %7s / %7s  ", bs, b1, b2);
        print_bar(use, tot);
        printf("   %5.1f%%  %4u page%s\n", pct_of(use, tot), pages, pages == 1 ? "" : "s");
    }

    printf("\n");
}

int cmd_start(int argc, char* argv[]) {
    /* line-buffered, so the start lines reach a log file before the daemon is stopped */
    setvbuf(stdout, NULL, _IOLBF, 0);

    Config c;
    config_defaults(&c);

    /* defaults, then the file, then the command line */
    const char* config_path = NULL;
    const char* port_arg    = NULL;
    for (int i = 2; i < argc; i++) {
        if (strcmp(argv[i], "--config") == 0 && i + 1 < argc) config_path = argv[++i];
        else if (strcmp(argv[i], "--port") == 0 && i + 1 < argc) port_arg = argv[++i];
    }

    if (config_path) config_load(&c, config_path);
    else             printf("no --config provided, using built-in defaults\n");
    if (port_arg)    c.port = atoi(port_arg);
    config_validate(&c);

    char features[96];
    pool_config_describe(&c.pool, features, sizeof(features));

    printf("quicx v%s starting\n", VERSION);
    printf("  port:    %d\n", c.port);
    printf("  pool:    %zu bytes, %u KiB pages given to block sizes on demand\n",
           c.pool.pool_size, (unsigned)(APOOL_PAGE_SIZE / 1024));
    printf("  apool:   %s\n", features);

    static Quicx quicx;
    ApoolStatus st = quicx_init(&quicx, &c);
    if (st != APOOL_OK) {
        fprintf(stderr, "error: cannot set up a %zu byte pool (status %d)", c.pool.pool_size, (int)st);
        if (c.pool.warm_pages != POOL_AUTO || c.pool.reserve_pages != POOL_AUTO)
            fprintf(stderr, " — are warm_pages and reserve_pages below the pool's page count?");
        fprintf(stderr, "\n");
        return 1;
    }
    printf("  pages:   %u for tasks (%u kept warm, %u reserved), %u for connections and workers\n",
           quicx.mem.num_pages, quicx.mem.warm, quicx.mem.reserve, quicx.ctrl.num_pages);

    FILE* pf = fopen(PID_FILE, "w");
    if (pf) {
        fprintf(pf, "%d\n", getpid());
        fclose(pf);
    }

    int r = server_start(c.port, &quicx);

    quicx_destroy(&quicx);
    unlink(PID_FILE);
    return r;
}

int cmd_stop(void) {
    FILE* f = fopen(PID_FILE, "r");
    if (!f) {
        fprintf(stderr, "quicx is not running\n");
        return 1;
    }

    int pid;
    fscanf(f, "%d", &pid);
    fclose(f);

    if (kill(pid, SIGTERM) < 0) {
        perror("kill");
        return 1;
    }

    unlink(PID_FILE);
    printf("quicx stopped (pid %d)\n", pid);
    return 0;
}

int cmd_status(void) {
    int fd = cli_connect();
    if (fd < 0) return 1;

    protocol_send_stats_request(fd);

    StatsHeader hdr;
    StatsClass  classes[APOOL_NUM_CLASSES];
    int n = 0;

    int r = cli_read_stats(fd, &hdr, classes, APOOL_NUM_CLASSES, &n);
    close(fd);

    if (r == CLI_STATS_VERSION_MISMATCH) {
        fprintf(stderr,
                "error: daemon speaks a different stats format\n"
                "       this is quicx v%s — restart the daemon to match\n",
                VERSION);
        return 1;
    }

    if (r < 0) {
        fprintf(stderr, "failed to read stats\n");
        return 1;
    }

    int pid = 0;
    FILE* pf = fopen(PID_FILE, "r");
    if (pf) {
        if (fscanf(pf, "%d", &pid) != 1) pid = 0;
        fclose(pf);
    }

    cli_print_stats(VERSION, pid, &hdr, classes, n);
    return 0;
}

int cmd_version(void) {
    printf("quicx v%s\n", VERSION);
    return 0;
}

void print_usage(void) {
    printf("quicx v%s — lightweight task queue daemon\n\n", VERSION);
    printf("usage:\n");
    printf("  quicx start --config FILE\n");
    printf("  quicx stop\n");
    printf("  quicx status\n");
    printf("  quicx version\n");
}
