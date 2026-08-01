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
#include "incPMAD.h"

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

    if ((MessageType)phdr.type != MSG_STATS_RESPONSE)
        return -1;

    if (protocol_read_exact(fd, hdr, sizeof(StatsHeader)) <= 0)
        return -1;

    uint32_t n = hdr->class_count;
    if ((int)n > max) return -1;

    if (n > 0)
        protocol_read_exact(fd, classes,
                            (int)(n * sizeof(StatsClass)));

    *count = (int)n;
    return 1;
}

void cli_print_stats(const char* version, StatsHeader* hdr,
                      StatsClass* classes, int n) {
    uint32_t idle      = ntohl(hdr->idle_workers);
    uint32_t busy      = ntohl(hdr->busy_workers);
    uint32_t queued    = ntohl(hdr->queued_tasks);
    uint64_t submitted = ntohll(hdr->tasks_submitted);
    uint64_t completed = ntohll(hdr->tasks_completed);
    uint64_t failed    = ntohll(hdr->tasks_failed);
    uint64_t uptime    = ntohll(hdr->uptime_seconds);

    uint64_t hours   = uptime / 3600;
    uint64_t minutes = (uptime % 3600) / 60;
    uint64_t seconds = uptime % 60;

    uint64_t total_bytes = 0;
    uint64_t used_bytes  = 0;

    for (int i = 0; i < n; i++) {
        uint32_t bs  = ntohl(classes[i].block_size);
        uint32_t tot = ntohl(classes[i].total_blocks);
        uint32_t use = ntohl(classes[i].allocated_blocks);
        total_bytes += (uint64_t)bs * tot;
        used_bytes  += (uint64_t)bs * use;
    }

    printf("\n");
    printf("  quicx v%s\n", version);
    printf("  ─────────────────────────────────────────\n");
    printf("  uptime     %lluh %llum %llus\n",
           (unsigned long long)hours,
           (unsigned long long)minutes,
           (unsigned long long)seconds);

    printf("\n");
    printf("  workers    idle: %-4u  busy: %-4u  total: %u\n",
           idle, busy, idle + busy);
    printf("  queue      waiting: %u\n", queued);

    printf("\n");
    printf("  tasks      submitted: %llu\n",
           (unsigned long long)submitted);
    printf("             completed: %llu\n",
           (unsigned long long)completed);
    printf("             failed:    %llu\n",
           (unsigned long long)failed);

    printf("\n");
    printf("  memory     %llu / %llu bytes (%.1f%%)\n",
           (unsigned long long)used_bytes,
           (unsigned long long)total_bytes,
           total_bytes > 0
               ? (double)used_bytes * 100.0 / (double)total_bytes
               : 0.0);

    printf("\n");
    printf("  PMAD:\n");

    for (int i = 0; i < n; i++) {
        uint32_t bs  = ntohl(classes[i].block_size);
        uint32_t tot = ntohl(classes[i].total_blocks);
        uint32_t use = ntohl(classes[i].allocated_blocks);

        int bar  = 20;
        int fill = tot > 0 ? (int)((uint64_t)use * bar / tot) : 0;

        printf("    %4uB  [", bs);
        for (int b = 0; b < bar; b++)
            printf("%s", b < fill ? "█" : "░");
        printf("]  %u / %u\n", use, tot);
    }

    printf("\n");
}

int cmd_start(int argc, char* argv[]) {
    Config c;
    config_defaults(&c);

    int config_provided = 0;
    for (int i = 2; i < argc; i++) {
        if (strcmp(argv[i], "--config") == 0 && i + 1 < argc) {
            config_load(&c, argv[++i]);
            config_provided = 1;
        } else if (strcmp(argv[i], "--port") == 0 && i + 1 < argc) {
            c.port = atoi(argv[++i]);
        }
    }

    if (!config_provided) {
        printf("no --config provided, using built-in defaults\n");
        config_validate(&c);
    }

    printf("quicx v%s starting\n", VERSION);
    printf("  port:    %d\n", c.port);
    printf("  classes:");
    for (int i = 0; i < c.class_count; i++)
        printf(" %zu", c.sizes[i]);
    printf("\n");

    pmad_init(c.sizes, c.class_count, c.percentages, c.pool_size);
    stats_init();

    static Queue q;
    static WorkerPool pool;

    queue_init(&q);
    workers_init(&pool);

    FILE* pf = fopen(PID_FILE, "w");
    if (pf) {
        fprintf(pf, "%d\n", getpid());
        fclose(pf);
    }

    int r = server_start(c.port, &q, &pool);

    pmad_destroy();
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
    StatsClass  classes[MAX_PMAD_CLASSES];
    int n = 0;

    int r = cli_read_stats(fd, &hdr, classes, MAX_PMAD_CLASSES, &n);
    close(fd);

    if (r < 0) {
        fprintf(stderr, "failed to read stats\n");
        return 1;
    }

    cli_print_stats(VERSION, &hdr, classes, n);
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
