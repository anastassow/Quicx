/*
bench_worker — a quicx worker that does real work.

registers with READY, then loops: receive TASK, parse the access-log line
in the payload, fold it into running per-status totals, reply DONE. one
task at a time, which is what the daemon's dispatcher assumes.

on SIGINT/SIGTERM it prints what it actually processed, so a run can be
checked for lost or duplicated tasks rather than trusted.
*/

#include "bench_common.h"
#include "workload.h"

#include <signal.h>

static volatile sig_atomic_t g_stop = 0;

static void on_signal(int sig) { (void)sig; g_stop = 1; }

typedef struct {
    const char* host;
    int         port;
    int         id;
    int         quiet;
    uint32_t    spin_us;     /* optional extra busy work per task */
} WorkerOpts;

static void usage(void) {
    fprintf(stderr,
        "usage: bench_worker [options]\n"
        "  --host HOST     daemon host (default 127.0.0.1)\n"
        "  --port PORT     daemon port (default 16381)\n"
        "  --id N          worker id, used in output only\n"
        "  --spin-us N     busy-spin N extra microseconds per task\n"
        "  --quiet         suppress the per-worker summary\n");
}

/* busy-spins for approximately us microseconds */
static void spin_us(uint32_t us) {
    if (!us) return;
    uint64_t deadline = now_ns() + (uint64_t)us * 1000ull;
    while (now_ns() < deadline) { /* deliberate busy wait */ }
}

int main(int argc, char* argv[]) {
    WorkerOpts o = { .host = "127.0.0.1", .port = 16381, .id = 0, .quiet = 0, .spin_us = 0 };

    for (int i = 1; i < argc; i++) {
        if      (!strcmp(argv[i], "--host")    && i + 1 < argc) o.host    = argv[++i];
        else if (!strcmp(argv[i], "--port")    && i + 1 < argc) o.port    = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--id")      && i + 1 < argc) o.id      = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--spin-us") && i + 1 < argc) o.spin_us = (uint32_t)atoi(argv[++i]);
        else if (!strcmp(argv[i], "--quiet"))                   o.quiet   = 1;
        else { usage(); return 1; }
    }

    install_stop_handler(SIGINT,  on_signal);
    install_stop_handler(SIGTERM, on_signal);
    signal(SIGPIPE, SIG_IGN);

    int fd = bench_connect(o.host, o.port);
    if (fd < 0) {
        fprintf(stderr, "worker %d: cannot connect to %s:%d\n", o.id, o.host, o.port);
        return 1;
    }

    if (frame_write(fd, MSG_READY, NULL, 0) < 0) {
        fprintf(stderr, "worker %d: failed to send READY\n", o.id);
        close(fd);
        return 1;
    }

    LogAggregate agg;
    memset(&agg, 0, sizeof(agg));

    uint64_t tasks     = 0;
    uint64_t bad_frame = 0;
    uint64_t busy_ns   = 0;

    uint8_t body[PROTO_MAX_PAYLOAD + 64];

    while (!g_stop) {
        uint8_t hdr[PROTO_HEADER_SIZE];
        int r = read_exact(fd, hdr, PROTO_HEADER_SIZE);
        if (r == READ_INTERRUPTED) continue;   /* loop re-checks g_stop */
        if (r <= 0) break;

        uint32_t len;
        memcpy(&len, hdr + 2, 4);
        len = ntohl(len);

        if (len > sizeof(body)) {
            fprintf(stderr, "worker %d: frame too big (%u)\n", o.id, len);
            break;
        }
        if (len > 0 && read_exact(fd, body, (int)len) <= 0) break;

        if (hdr[1] != MSG_TASK) {
            /* WAIT, PONG and friends need no reply */
            continue;
        }

        /* TASK body: [task_id be32][type_len u8][type][payload] */
        if (len < 5) { bad_frame++; continue; }

        uint32_t task_id;
        memcpy(&task_id, body, 4);
        task_id = ntohl(task_id);

        uint8_t type_len = body[4];
        if (5u + type_len > len) { bad_frame++; continue; }

        char type[PROTO_MAX_TYPE_LEN + 1];
        if (type_len > PROTO_MAX_TYPE_LEN) { bad_frame++; continue; }
        memcpy(type, body + 5, type_len);
        type[type_len] = '\0';

        const char* payload     = (const char*)(body + 5 + type_len);
        uint32_t    payload_len = len - 5u - type_len;

        uint64_t t0 = now_ns();
        workload_process(type, payload, payload_len, &agg);
        spin_us(o.spin_us);
        busy_ns += now_ns() - t0;

        tasks++;

        if (frame_write(fd, MSG_DONE, (const uint8_t*)&(uint32_t){ htonl(task_id) }, 4) < 0) break;
    }

    close(fd);

    if (!o.quiet) {
        fprintf(stderr,
            "worker %-3d tasks=%llu lines=%llu parse_fail=%llu bad_frames=%llu "
            "busy=%.1fms 2xx=%llu 3xx=%llu 4xx=%llu 5xx=%llu\n",
            o.id,
            (unsigned long long)tasks,
            (unsigned long long)agg.lines,
            (unsigned long long)agg.parse_failures,
            (unsigned long long)bad_frame,
            (double)busy_ns / 1e6,
            (unsigned long long)agg.by_class[2],
            (unsigned long long)agg.by_class[3],
            (unsigned long long)agg.by_class[4],
            (unsigned long long)agg.by_class[5]);
    }

    return 0;
}
