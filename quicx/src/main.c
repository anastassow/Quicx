#if defined(__linux__)
#define _DEFAULT_SOURCE
#define _BSD_SOURCE
#define _GNU_SOURCE
#endif
#if defined(__APPLE__)
#define _DARWIN_C_SOURCE
#endif

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
#include <ctype.h>

#if defined(__linux__)
#include <endian.h>
#ifndef ntohll
#define ntohll(x) be64toh(x)
#endif
#endif

#include "queue.h"
#include "workers.h"
#include "stats.h"
#include "server.h"
#include "protocol.h"
#include "incPMAD.h"

#define VERSION          "1.0.0"
#define DEFAULT_PORT     16381
#define UNIX_SOCKET_PATH "/tmp/quicx.sock"
#define PID_FILE         "/tmp/quicx.pid"

// config ────────────────────────────────────────────────────────────────────

typedef struct {
    int    port;
    size_t pool_size;
    size_t sizes[16];
    size_t percentages[16];
    int    class_count;
} Config;

static char* trim(char* s) {
    while (isspace((unsigned char)*s)) s++;
    if (*s == 0) return s;

    char* end = s + strlen(s) - 1;
    while (end > s && isspace((unsigned char)*end)) end--;
    end[1] = '\0';
    return s;
}

static void config_defaults(Config* c) {
    c->port        = DEFAULT_PORT;
    c->pool_size   = 1048576;
    c->class_count = 6;

    c->sizes[0] = 32;   c->percentages[0] = 10;
    c->sizes[1] = 64;   c->percentages[1] = 25;
    c->sizes[2] = 128;  c->percentages[2] = 25;
    c->sizes[3] = 256;  c->percentages[3] = 20;
    c->sizes[4] = 512;  c->percentages[4] = 12;
    c->sizes[5] = 1024; c->percentages[5] =  8;
}

static void config_validate(Config* c) {
    if (c->class_count == 0) {
        fprintf(stderr, "error: no allocator classes defined\n");
        exit(1);
    }

    int sum = 0;
    for (int i = 0; i < c->class_count; i++)
        sum += c->percentages[i];

    if (c->pool_size == 0) {
        fprintf(stderr, "error: pool_size must be greater than 0\n");
        exit(1);
    }
    
    if (sum != 100) {
        fprintf(stderr, "error: class percentages must sum to 100 (got %d)\n", sum);
        exit(1);
    }
}

static void config_load(Config* c, const char* path) {
    FILE* f = fopen(path, "r");
    if (!f) {
        fprintf(stderr, "error: cannot open config: %s\n", path);
        exit(1);
    }

    char line[512];

    while (fgets(line, sizeof(line), f)) {
        char* l = trim(line);

        if (l[0] == '#' || l[0] == '\0')
            continue;

        if (l[0] == '[')
            continue;

        char key[64], val[256];

        if (sscanf(l, "%63[^=]=%255[^\n]", key, val) != 2)
            continue;

        char* k = trim(key);
        char* v = trim(val);

        if (strcmp(k, "port") == 0) {
            c->port = atoi(v);
        }
        else if (strcmp(k, "pool_size") == 0) {
            c->pool_size = (size_t)atoll(v);
        }
        else if (strcmp(k, "class") == 0) {
            if (c->class_count >= 16) {
                fprintf(stderr, "error: too many classes (max 16)\n");
                exit(1);
            }

            size_t size = 0, percent = 0;

            if (sscanf(v, "%zu,%zu", &size, &percent) != 2) {
                fprintf(stderr, "error: invalid class format: %s\n", v);
                exit(1);
            }

            c->sizes[c->class_count]       = size;
            c->percentages[c->class_count] = percent;
            c->class_count++;
        }
    }

    fclose(f);
    config_validate(c);

    printf("config loaded: %s\n", path);
}

// CLI helpers ───────────────────────────────────────────────────────────────

static int cli_connect(void) {
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

static int cli_read_stats(int fd, StatsHeader* hdr,
                         StatsClass* classes, int max,
                         int* count) {
    ProtocolHeader phdr;
    if (protocol_read_exact(fd, &phdr, PROTOCOL_HEADER_SIZE) <= 0)
        return -1;

    if ((MessageType)phdr.type != MSG_STATS_RESPONSE)
        return -1;

    if (protocol_read_exact(fd, hdr, sizeof(StatsHeader)) <= 0)
        return -1;

    uint32_t n = ntohl(hdr->class_count);
    if ((int)n > max) return -1;

    if (n > 0)
        protocol_read_exact(fd, classes,
                            (int)(n * sizeof(StatsClass)));

    *count = (int)n;
    return 1;
}

static void cli_print_stats(StatsHeader* hdr,
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
    printf("  quicx v%s\n", VERSION);
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

// commands ──────────────────────────────────────────────────────────────────

static int cmd_start(int argc, char* argv[]) {
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

static int cmd_stop(void) {
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

static int cmd_status(void) {
    int fd = cli_connect();
    if (fd < 0) return 1;

    protocol_send_stats_request(fd);

    StatsHeader hdr;
    StatsClass  classes[16];
    int n = 0;

    int r = cli_read_stats(fd, &hdr, classes, 16, &n);
    close(fd);

    if (r < 0) {
        fprintf(stderr, "failed to read stats\n");
        return 1;
    }

    cli_print_stats(&hdr, classes, n);
    return 0;
}


static int cmd_version(void) {
    printf("quicx v%s\n", VERSION);
    return 0;
}

static void print_usage(void) {
    printf("quicx v%s — lightweight task queue daemon\n\n", VERSION);
    printf("usage:\n");
    printf("  quicx start --config FILE\n");
    printf("  quicx stop\n");
    printf("  quicx status\n");
    printf("  quicx version\n");
}

// main ──────────────────────────────────────────────────────────────────────

int main(int argc, char* argv[]) {
    if (argc < 2) {
        print_usage();
        return 1;
    }

    if (strcmp(argv[1], "start") == 0)   return cmd_start(argc, argv);
    if (strcmp(argv[1], "stop") == 0)    return cmd_stop();
    if (strcmp(argv[1], "status") == 0)  return cmd_status();
    if (strcmp(argv[1], "version") == 0) return cmd_version();

    fprintf(stderr, "unknown command: %s\n\n", argv[1]);
    print_usage();
    return 1;
}