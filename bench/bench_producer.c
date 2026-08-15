/*
bench_producer — load generator and latency recorder.

drives N connections from one poll loop and measures end-to-end latency:
the time from when a task was *due to be sent* until its DONE comes back.
that is the number a task queue is actually judged on. the existing
python harness measures submit->OK, which only times the daemon's ack and
never touches the worker round trip.

two modes:

  --rate R      open loop. tasks are due on a fixed schedule and latency
                is measured from the scheduled time, not the actual send.
                if the daemon stalls, the backlog shows up in the numbers
                instead of being hidden by the client slowing down. this
                is the coordinated-omission correction and it is why p99
                here will look worse (and truer) than a closed-loop p99.

  --inflight N  closed loop. keep N tasks outstanding per connection and
                go as fast as the daemon allows. use this for peak
                throughput, not for latency claims.

correlation: the daemon assigns task ids and returns them in OK, in the
same order the submits were sent on that connection, so each connection
keeps a FIFO of scheduled timestamps and pairs them with incoming OKs.
DONE then looks the timestamp up by id.
*/

#include "bench_common.h"
#include "workload.h"

#include <poll.h>
#include <signal.h>

#define ID_TABLE_BITS 20
#define ID_TABLE_SIZE (1u << ID_TABLE_BITS)
#define ID_TABLE_MASK (ID_TABLE_SIZE - 1u)

#define RBUF_SIZE (64u  * 1024u)
#define WBUF_SIZE (256u * 1024u)
#define PEND_CAP  (1u   << 16)
#define PEND_MASK (PEND_CAP - 1u)

static volatile sig_atomic_t g_stop = 0;
static void on_signal(int sig) { (void)sig; g_stop = 1; }

typedef struct {
    int      fd;
    int      alive;

    uint8_t  rbuf[RBUF_SIZE];
    uint32_t rlen;

    uint8_t  wbuf[WBUF_SIZE];
    uint32_t wlen;
    uint32_t woff;

    /* scheduled send times awaiting their OK, in send order */
    uint64_t pend[PEND_CAP];
    uint32_t pend_head;
    uint32_t pend_tail;

    uint32_t inflight;
    uint64_t next_due_ns;

    uint64_t sent;
    uint64_t oks;
    uint64_t dones;
    uint64_t errors;

    Rng      rng;
    uint32_t type_cursor;
} PConn;

typedef struct {
    const char* host;
    int         port;
    int         conns;
    double      rate;         /* total tasks/sec across conns; 0 = closed loop */
    int         inflight;     /* closed-loop target per connection */
    double      duration_s;
    double      warmup_s;
    const char* out_path;
    uint64_t    seed;
    int         quiet;
} Opts;

static uint64_t* g_id_ts;      /* task id -> scheduled send time */
static Hist      g_hist;
static uint64_t  g_err_code[8];  /* MSG_ERROR frames, counted by ErrorCode */

static const char* err_name(int code) {
    switch (code) {
        case 1: return "QUEUE_FULL";
        case 2: return "INVALID_MSG";
        case 3: return "PAYLOAD_TOO_BIG";
        case 4: return "UNKNOWN_TYPE";
        case 5: return "VERSION_MISMATCH";
        default: return "?";
    }
}

static void usage(void) {
    fprintf(stderr,
        "usage: bench_producer [options]\n"
        "  --host HOST        daemon host (default 127.0.0.1)\n"
        "  --port PORT        daemon port (default 16381)\n"
        "  --conns N          connections in this process (default 1)\n"
        "  --rate R           total tasks/sec, open loop (default: closed loop)\n"
        "  --inflight N       closed-loop outstanding tasks per conn (default 32)\n"
        "  --duration S       seconds to run, warmup included (default 10)\n"
        "  --warmup S         seconds excluded from the histogram (default 2)\n"
        "  --out FILE         write the raw histogram here for merging\n"
        "  --seed N           rng seed (default 1)\n"
        "  --quiet            no per-process summary\n");
}

static void conn_init(PConn* c, const Opts* o, int index, uint64_t start_ns) {
    memset(c, 0, sizeof(*c));
    c->fd = bench_connect(o->host, o->port);
    if (c->fd < 0) {
        fprintf(stderr, "producer: connection %d failed\n", index);
        exit(1);
    }
    set_nonblocking(c->fd);
    c->alive = 1;
    rng_seed(&c->rng, o->seed * 1000003ull + (uint64_t)index + 1);

    /* stagger the schedules so N connections do not fire in lockstep */
    if (o->rate > 0.0) {
        double per_conn   = o->rate / (double)o->conns;
        double interval_ns = 1e9 / per_conn;
        c->next_due_ns = start_ns + (uint64_t)(interval_ns * (double)index / (double)o->conns);
    } else {
        c->next_due_ns = start_ns;
    }
}

/* queues one SUBMIT into the write buffer. returns 0 on success. */
static int queue_submit(PConn* c, uint64_t due_ns) {
    char     line[PROTO_MAX_PAYLOAD];
    uint32_t line_len = workload_gen_line(line, sizeof(line), &c->rng);

    const char* type = WL_TYPES[c->type_cursor++ % WL_TYPE_COUNT];

    uint8_t  body[PROTO_MAX_PAYLOAD + 64];
    uint32_t body_len = encode_submit(body, type, (const uint8_t*)line, line_len);

    uint32_t frame_len = PROTO_HEADER_SIZE + body_len;
    if (c->wlen + frame_len > WBUF_SIZE) return -1;

    uint8_t* p = c->wbuf + c->wlen;
    p[0] = PROTO_VERSION;
    p[1] = MSG_SUBMIT;
    uint32_t net_len = htonl(body_len);
    memcpy(p + 2, &net_len, 4);
    memcpy(p + PROTO_HEADER_SIZE, body, body_len);
    c->wlen += frame_len;

    uint32_t slot = c->pend_tail & PEND_MASK;
    if (((c->pend_tail + 1) & PEND_MASK) == (c->pend_head & PEND_MASK)) return -1;  /* fifo full */
    c->pend[slot] = due_ns;
    c->pend_tail++;

    c->inflight++;
    c->sent++;
    return 0;
}

static void conn_flush(PConn* c) {
    while (c->woff < c->wlen) {
        ssize_t n = write(c->fd, c->wbuf + c->woff, c->wlen - c->woff);
        if (n < 0) {
            if (errno == EINTR)  continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) return;
            c->alive = 0;
            return;
        }
        if (n == 0) { c->alive = 0; return; }
        c->woff += (uint32_t)n;
    }
    c->wlen = 0;
    c->woff = 0;
}

/* consumes whole frames out of rbuf. */
static void conn_parse(PConn* c, uint64_t measure_start_ns) {
    uint32_t off = 0;

    while (c->rlen - off >= PROTO_HEADER_SIZE) {
        const uint8_t* h = c->rbuf + off;

        uint32_t len;
        memcpy(&len, h + 2, 4);
        len = ntohl(len);

        if (len > PROTO_MAX_PAYLOAD + 64) { c->alive = 0; return; }
        if (c->rlen - off < PROTO_HEADER_SIZE + len) break;   /* partial, wait for more */

        const uint8_t* body = h + PROTO_HEADER_SIZE;
        uint8_t        type = h[1];

        if (type == MSG_OK && len >= 4) {
            uint32_t id;
            memcpy(&id, body, 4);
            id = ntohl(id);

            if (c->pend_head != c->pend_tail) {
                uint64_t due = c->pend[c->pend_head & PEND_MASK];
                c->pend_head++;
                g_id_ts[id & ID_TABLE_MASK] = due;
            }
            c->oks++;

        } else if (type == MSG_DONE && len >= 4) {
            uint32_t id;
            memcpy(&id, body, 4);
            id = ntohl(id);

            uint64_t due = g_id_ts[id & ID_TABLE_MASK];
            if (due != 0) {
                g_id_ts[id & ID_TABLE_MASK] = 0;
                if (due >= measure_start_ns) hist_add(&g_hist, now_ns() - due);
            }
            c->dones++;
            if (c->inflight) c->inflight--;

        } else if (type == MSG_ERROR) {
            if (len >= 1 && body[0] < 8) g_err_code[body[0]]++;
            c->errors++;
            if (c->inflight) c->inflight--;
            if (c->pend_head != c->pend_tail) c->pend_head++;
        }

        off += PROTO_HEADER_SIZE + len;
    }

    if (off) {
        memmove(c->rbuf, c->rbuf + off, c->rlen - off);
        c->rlen -= off;
    }
}

static void conn_read(PConn* c, uint64_t measure_start_ns) {
    for (;;) {
        if (c->rlen == RBUF_SIZE) { conn_parse(c, measure_start_ns); if (c->rlen == RBUF_SIZE) { c->alive = 0; return; } }

        ssize_t n = read(c->fd, c->rbuf + c->rlen, RBUF_SIZE - c->rlen);
        if (n < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
            c->alive = 0;
            return;
        }
        if (n == 0) { c->alive = 0; return; }
        c->rlen += (uint32_t)n;
    }
    conn_parse(c, measure_start_ns);
}

int main(int argc, char* argv[]) {
    Opts o = { .host = "127.0.0.1", .port = 16381, .conns = 1, .rate = 0.0,
               .inflight = 32, .duration_s = 10.0, .warmup_s = 2.0,
               .out_path = NULL, .seed = 1, .quiet = 0 };

    for (int i = 1; i < argc; i++) {
        if      (!strcmp(argv[i], "--host")     && i + 1 < argc) o.host       = argv[++i];
        else if (!strcmp(argv[i], "--port")     && i + 1 < argc) o.port       = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--conns")    && i + 1 < argc) o.conns      = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--rate")     && i + 1 < argc) o.rate       = atof(argv[++i]);
        else if (!strcmp(argv[i], "--inflight") && i + 1 < argc) o.inflight   = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--duration") && i + 1 < argc) o.duration_s = atof(argv[++i]);
        else if (!strcmp(argv[i], "--warmup")   && i + 1 < argc) o.warmup_s   = atof(argv[++i]);
        else if (!strcmp(argv[i], "--out")      && i + 1 < argc) o.out_path   = argv[++i];
        else if (!strcmp(argv[i], "--seed")     && i + 1 < argc) o.seed       = strtoull(argv[++i], NULL, 10);
        else if (!strcmp(argv[i], "--quiet"))                    o.quiet      = 1;
        else { usage(); return 1; }
    }

    if (o.conns < 1) { usage(); return 1; }

    signal(SIGINT,  on_signal);
    signal(SIGTERM, on_signal);
    signal(SIGPIPE, SIG_IGN);

    g_id_ts = calloc(ID_TABLE_SIZE, sizeof(uint64_t));
    if (!g_id_ts) { fprintf(stderr, "producer: out of memory\n"); return 1; }
    hist_init(&g_hist);

    PConn* conns = calloc((size_t)o.conns, sizeof(PConn));
    if (!conns) { fprintf(stderr, "producer: out of memory\n"); return 1; }

    uint64_t start_ns = now_ns();
    for (int i = 0; i < o.conns; i++) conn_init(&conns[i], &o, i, start_ns);

    /* connections are up; restart the clock so connect time is not in the window */
    start_ns = now_ns();
    uint64_t measure_start_ns = start_ns + (uint64_t)(o.warmup_s   * 1e9);
    uint64_t end_ns           = start_ns + (uint64_t)(o.duration_s * 1e9);

    double interval_ns = (o.rate > 0.0)
                       ? 1e9 / (o.rate / (double)o.conns)
                       : 0.0;

    struct pollfd* pfd = calloc((size_t)o.conns, sizeof(struct pollfd));
    if (!pfd) { fprintf(stderr, "producer: out of memory\n"); return 1; }

    uint64_t measured_start_ns = 0;   /* wall clock of the measured window, for throughput */
    uint64_t done_at_measure   = 0;
    int      measure_latched   = 0;

    while (!g_stop) {
        uint64_t now = now_ns();
        if (now >= end_ns) break;

        if (!measure_latched && now >= measure_start_ns) {
            measured_start_ns = now;
            for (int i = 0; i < o.conns; i++) done_at_measure += conns[i].dones;
            measure_latched = 1;
        }

        /* queue whatever is due */
        uint64_t next_wake = end_ns;
        int      any_alive = 0;

        for (int i = 0; i < o.conns; i++) {
            PConn* c = &conns[i];
            if (!c->alive) continue;
            any_alive = 1;

            if (o.rate > 0.0) {
                int budget = 512;   /* bound the work done for one conn per pass */
                while (now >= c->next_due_ns && budget-- > 0) {
                    if (queue_submit(c, c->next_due_ns) != 0) break;
                    c->next_due_ns += (uint64_t)interval_ns;
                }
                if (c->next_due_ns < next_wake) next_wake = c->next_due_ns;
            } else {
                while (c->inflight < (uint32_t)o.inflight) {
                    if (queue_submit(c, now_ns()) != 0) break;
                }
                next_wake = now;   /* closed loop never idles */
            }

            conn_flush(c);
        }

        if (!any_alive) break;

        int nfds = 0;
        for (int i = 0; i < o.conns; i++) {
            if (!conns[i].alive) continue;
            pfd[nfds].fd      = conns[i].fd;
            pfd[nfds].events  = POLLIN | (conns[i].woff < conns[i].wlen ? POLLOUT : 0);
            pfd[nfds].revents = 0;
            nfds++;
        }
        if (nfds == 0) break;

        int timeout_ms = 0;
        if (next_wake > now) {
            uint64_t d = next_wake - now;
            timeout_ms = (int)(d / 1000000ull);
            if (timeout_ms > 10) timeout_ms = 10;   /* stay responsive to replies */
        }

        int pn = poll(pfd, (nfds_t)nfds, timeout_ms);
        if (pn < 0) {
            if (errno == EINTR) continue;
            perror("poll");
            break;
        }

        int k = 0;
        for (int i = 0; i < o.conns; i++) {
            if (!conns[i].alive) continue;
            short re = pfd[k++].revents;
            if (re & (POLLIN | POLLHUP | POLLERR)) conn_read(&conns[i], measure_start_ns);
            if (conns[i].alive && (re & POLLOUT))  conn_flush(&conns[i]);
        }
    }

    uint64_t stop_ns = now_ns();

    /* drain briefly so in-flight DONEs are not counted as losses */
    uint64_t drain_until = stop_ns + 500000000ull;
    while (now_ns() < drain_until) {
        int nfds = 0;
        for (int i = 0; i < o.conns; i++) {
            if (!conns[i].alive) continue;
            pfd[nfds].fd      = conns[i].fd;
            pfd[nfds].events  = POLLIN;
            pfd[nfds].revents = 0;
            nfds++;
        }
        if (nfds == 0) break;

        int pn = poll(pfd, (nfds_t)nfds, 50);
        if (pn <= 0) {
            if (pn < 0 && errno == EINTR) continue;
            int outstanding = 0;
            for (int i = 0; i < o.conns; i++) outstanding += (int)conns[i].inflight;
            if (outstanding == 0) break;
            continue;
        }

        int k = 0;
        for (int i = 0; i < o.conns; i++) {
            if (!conns[i].alive) continue;
            if (pfd[k++].revents & (POLLIN | POLLHUP | POLLERR))
                conn_read(&conns[i], measure_start_ns);
        }
    }

    uint64_t sent = 0, oks = 0, dones = 0, errors = 0, dropped = 0;
    for (int i = 0; i < o.conns; i++) {
        sent   += conns[i].sent;
        oks    += conns[i].oks;
        dones  += conns[i].dones;
        errors += conns[i].errors;
        if (!conns[i].alive) dropped++;
        close(conns[i].fd);
    }

    double measured_s = measure_latched
                      ? (double)(stop_ns - measured_start_ns) / 1e9
                      : 0.0;
    double throughput = measured_s > 0.0
                      ? (double)(dones - done_at_measure) / measured_s
                      : 0.0;

    if (o.out_path && hist_write(&g_hist, o.out_path) != 0)
        fprintf(stderr, "producer: could not write %s\n", o.out_path);

    if (!o.quiet) {
        char tb[32];
        fmt_count(tb, sizeof(tb), throughput);
        fprintf(stderr,
            "producer  sent=%llu ok=%llu done=%llu err=%llu dropped_conns=%llu "
            "measured=%.2fs thr=%s/s samples=%llu\n",
            (unsigned long long)sent, (unsigned long long)oks,
            (unsigned long long)dones, (unsigned long long)errors,
            (unsigned long long)dropped, measured_s, tb,
            (unsigned long long)g_hist.count);

        for (int i = 1; i < 8; i++) {
            if (g_err_code[i])
                fprintf(stderr, "          error %-16s x%llu\n",
                        err_name(i), (unsigned long long)g_err_code[i]);
        }
    }

    /* a nonzero exit tells the runner the numbers are not trustworthy */
    int bad = (dropped > 0) || (errors > 0);

    free(pfd);
    free(conns);
    free(g_id_ts);
    return bad ? 2 : 0;
}
