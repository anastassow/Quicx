#ifndef BENCH_COMMON_H
#define BENCH_COMMON_H

/*
shared helpers for the quicx benchmark clients: wire framing, a
monotonic clock, and a fixed-resolution latency histogram.

the clients speak the protocol directly rather than linking quicx's
protocol.c on purpose — a benchmark that shares a codec with the thing
it measures cannot catch a codec bug, and the producer needs partial
read handling that protocol_read_exact does not do.
*/

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <arpa/inet.h>
#include <netinet/tcp.h>
#include <sys/socket.h>

#define PROTO_VERSION      1
#define PROTO_HEADER_SIZE  6
#define PROTO_MAX_PAYLOAD  1024
#define PROTO_MAX_TYPE_LEN 32

#define MSG_SUBMIT 0x01
#define MSG_OK     0x02
#define MSG_ERROR  0x03
#define MSG_READY  0x04
#define MSG_DONE   0x05
#define MSG_FAILED 0x06
#define MSG_TASK   0x07
#define MSG_WAIT   0x08

/* clock */

static inline uint64_t now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

/* histogram — 1us buckets up to 65ms, everything above lands in overflow */

#define HIST_BUCKETS 65536

typedef struct {
    uint64_t bucket[HIST_BUCKETS];
    uint64_t overflow;
    uint64_t count;
    uint64_t sum_ns;
    uint64_t min_ns;
    uint64_t max_ns;
} Hist;

static inline void hist_init(Hist* h) {
    memset(h, 0, sizeof(*h));
    h->min_ns = UINT64_MAX;
}

static inline void hist_add(Hist* h, uint64_t ns) {
    uint64_t us = ns / 1000;
    if (us < HIST_BUCKETS) h->bucket[us]++;
    else                   h->overflow++;

    h->count++;
    h->sum_ns += ns;
    if (ns < h->min_ns) h->min_ns = ns;
    if (ns > h->max_ns) h->max_ns = ns;
}

static inline void hist_merge(Hist* dst, const Hist* src) {
    for (int i = 0; i < HIST_BUCKETS; i++) dst->bucket[i] += src->bucket[i];
    dst->overflow += src->overflow;
    dst->count    += src->count;
    dst->sum_ns   += src->sum_ns;
    if (src->min_ns < dst->min_ns) dst->min_ns = src->min_ns;
    if (src->max_ns > dst->max_ns) dst->max_ns = src->max_ns;
}

/*
returns the requested percentile in microseconds. samples that landed in
the overflow bucket are reported as the observed max, so a percentile that
falls inside overflow is a lower bound rather than a wrong small number.
*/
static inline double hist_pct(const Hist* h, double p) {
    if (h->count == 0) return 0.0;

    uint64_t target = (uint64_t)((double)h->count * p / 100.0);
    if (target == 0) target = 1;

    uint64_t seen = 0;
    for (int i = 0; i < HIST_BUCKETS; i++) {
        seen += h->bucket[i];
        if (seen >= target) return (double)i;
    }
    return (double)h->max_ns / 1000.0;
}

static inline int hist_write(const Hist* h, const char* path) {
    FILE* f = fopen(path, "wb");
    if (!f) return -1;
    size_t n = fwrite(h, sizeof(*h), 1, f);
    fclose(f);
    return n == 1 ? 0 : -1;
}

static inline int hist_read(Hist* h, const char* path) {
    FILE* f = fopen(path, "rb");
    if (!f) return -1;
    size_t n = fread(h, sizeof(*h), 1, f);
    fclose(f);
    return n == 1 ? 0 : -1;
}

/* sockets */

static inline int bench_connect(const char* host, int port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port   = htons((uint16_t)port);
    if (inet_pton(AF_INET, host, &addr.sin_addr) != 1) { close(fd); return -1; }

    /* the daemon may not be listening yet when a runner starts everything at once */
    for (int attempt = 0; attempt < 100; attempt++) {
        if (connect(fd, (struct sockaddr*)&addr, sizeof(addr)) == 0) {
            int one = 1;
            setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
            return fd;
        }
        if (errno != ECONNREFUSED) break;
        usleep(50000);
    }
    close(fd);
    return -1;
}

static inline int set_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) return -1;
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

/* framing */

/* writes a complete frame, looping over short writes. blocking sockets only. */
static inline int frame_write(int fd, uint8_t type, const uint8_t* body, uint32_t len) {
    uint8_t buf[PROTO_HEADER_SIZE + PROTO_MAX_PAYLOAD + 64];
    if (len > sizeof(buf) - PROTO_HEADER_SIZE) return -1;

    buf[0] = PROTO_VERSION;
    buf[1] = type;
    uint32_t net_len = htonl(len);
    memcpy(buf + 2, &net_len, 4);
    if (len) memcpy(buf + PROTO_HEADER_SIZE, body, len);

    uint32_t total = PROTO_HEADER_SIZE + len;
    uint32_t sent  = 0;
    while (sent < total) {
        ssize_t n = write(fd, buf + sent, total - sent);
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (n == 0) return -1;
        sent += (uint32_t)n;
    }
    return 0;
}

/*
reads exactly n bytes on a blocking socket. returns n, 0 on clean
disconnect, -1 on error, or READ_INTERRUPTED if a signal arrived before
any byte of this frame was consumed — the caller can safely stop there.
once part of a frame has been read the loop keeps going, because
abandoning it mid-frame would desynchronize the stream.
*/
#define READ_INTERRUPTED (-2)

static inline int read_exact(int fd, void* buf, int n) {
    int total = 0;
    uint8_t* p = (uint8_t*)buf;
    while (total < n) {
        ssize_t r = read(fd, p + total, (size_t)(n - total));
        if (r == 0) return 0;
        if (r < 0) {
            if (errno == EINTR) {
                if (total == 0) return READ_INTERRUPTED;
                continue;
            }
            return -1;
        }
        total += (int)r;
    }
    return total;
}

/* installs a handler that does NOT restart syscalls, so blocking reads wake */
static inline void install_stop_handler(int sig, void (*fn)(int)) {
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = fn;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;                /* deliberately no SA_RESTART */
    sigaction(sig, &sa, NULL);
}

/* encodes a SUBMIT body: [type_len u8][type][payload] */
static inline uint32_t encode_submit(uint8_t* out, const char* type,
                                     const uint8_t* payload, uint32_t payload_len) {
    uint8_t type_len = (uint8_t)strlen(type);
    out[0] = type_len;
    memcpy(out + 1, type, type_len);
    if (payload_len) memcpy(out + 1 + type_len, payload, payload_len);
    return 1u + type_len + payload_len;
}

static inline void fmt_count(char* buf, size_t cap, double v) {
    if      (v >= 1e9) snprintf(buf, cap, "%.2fB", v / 1e9);
    else if (v >= 1e6) snprintf(buf, cap, "%.2fM", v / 1e6);
    else if (v >= 1e3) snprintf(buf, cap, "%.1fk", v / 1e3);
    else               snprintf(buf, cap, "%.0f", v);
}

#endif
