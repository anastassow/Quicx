#ifndef WORKLOAD_H
#define WORKLOAD_H

/*
the benchmark's use case: HTTP access-log enrichment.

producers emit real nginx combined-format log lines. workers parse them
into fields, classify the request and accumulate per-status counters —
the same shape of work a real log pipeline does. nothing here is a
sleep or a spin loop; the cost is genuine parsing over genuine bytes.

payloads land between roughly 120 and 260 bytes, which is what this
workload actually produces and comfortably under PROTOCOL_MAX_PAYLOAD.
*/

#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* xorshift64* — deterministic per-seed so runs are reproducible */
typedef struct { uint64_t s; } Rng;

static inline void rng_seed(Rng* r, uint64_t seed) {
    r->s = seed ? seed : 0x9E3779B97F4A7C15ull;
}

static inline uint64_t rng_next(Rng* r) {
    uint64_t x = r->s;
    x ^= x >> 12;
    x ^= x << 25;
    x ^= x >> 27;
    r->s = x;
    return x * 0x2545F4914F6CDD1Dull;
}

static inline uint32_t rng_below(Rng* r, uint32_t n) {
    return (uint32_t)(rng_next(r) % n);
}

static const char* const WL_PATHS[] = {
    "/", "/index.html", "/api/v1/orders", "/api/v1/orders/48213",
    "/api/v1/users/2213/profile", "/static/js/app.4f2a1c.js",
    "/static/css/main.9de1.css", "/images/hero-banner-2x.png",
    "/api/v2/search?q=running+shoes&page=3", "/checkout/confirm",
    "/api/v1/cart/items", "/health", "/metrics",
    "/api/v1/products/94812/reviews?sort=recent",
    "/assets/fonts/inter-var.woff2", "/login",
};

static const char* const WL_METHODS[] = { "GET", "GET", "GET", "GET", "POST", "POST", "PUT", "DELETE" };
static const int         WL_STATUS[]  = { 200, 200, 200, 200, 200, 301, 304, 404, 500, 503 };

static const char* const WL_AGENTS[] = {
    "Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/124.0 Safari/537.36",
    "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/123.0 Safari/537.36",
    "Mozilla/5.0 (iPhone; CPU iPhone OS 17_4 like Mac OS X) AppleWebKit/605.1.15 Version/17.4 Mobile/15E148",
    "curl/8.4.0",
    "Go-http-client/2.0",
};

static const char* const WL_REFERRERS[] = {
    "-", "https://example.com/checkout", "https://www.google.com/",
    "https://example.com/products", "https://t.co/aBcD1234",
};

/* task types — a mixed workload so per-type cost differences are visible */
static const char* const WL_TYPES[] = { "log.parse", "log.enrich", "log.audit" };
#define WL_TYPE_COUNT 3

#define WL_ARRAY_LEN(a) ((uint32_t)(sizeof(a) / sizeof((a)[0])))

/* builds one nginx combined-format line. returns its length. */
static inline uint32_t workload_gen_line(char* buf, size_t cap, Rng* r) {
    uint32_t a = rng_below(r, 223) + 1;
    uint32_t b = rng_below(r, 256);
    uint32_t c = rng_below(r, 256);
    uint32_t d = rng_below(r, 254) + 1;

    const char* method = WL_METHODS[rng_below(r, WL_ARRAY_LEN(WL_METHODS))];
    const char* path   = WL_PATHS[rng_below(r, WL_ARRAY_LEN(WL_PATHS))];
    int         status = WL_STATUS[rng_below(r, WL_ARRAY_LEN(WL_STATUS))];
    uint32_t    bytes  = rng_below(r, 48000) + 120;
    const char* ref    = WL_REFERRERS[rng_below(r, WL_ARRAY_LEN(WL_REFERRERS))];
    const char* ua     = WL_AGENTS[rng_below(r, WL_ARRAY_LEN(WL_AGENTS))];

    int hh = (int)rng_below(r, 24), mm = (int)rng_below(r, 60), ss = (int)rng_below(r, 60);

    int n = snprintf(buf, cap,
        "%u.%u.%u.%u - - [15/Aug/2026:%02d:%02d:%02d +0000] "
        "\"%s %s HTTP/1.1\" %d %u \"%s\" \"%s\"",
        a, b, c, d, hh, mm, ss, method, path, status, bytes, ref, ua);

    if (n < 0) return 0;
    if ((size_t)n > cap - 1) n = (int)cap - 1;
    return (uint32_t)n;
}

/* what a worker extracts from a line */
typedef struct {
    uint32_t ip;
    int      status;
    uint32_t bytes;
    uint32_t path_hash;
    uint8_t  method_id;
    uint8_t  ok;
} LogRecord;

/* running totals a worker keeps across the tasks it handles */
typedef struct {
    uint64_t lines;
    uint64_t parse_failures;
    uint64_t bytes_total;
    uint64_t by_class[6];   /* index 0 unused, then 1xx..5xx */
    uint32_t hash_acc;
} LogAggregate;

static inline uint8_t wl_method_id(const char* s, size_t len) {
    if (len == 3 && memcmp(s, "GET", 3) == 0)    return 1;
    if (len == 4 && memcmp(s, "POST", 4) == 0)   return 2;
    if (len == 3 && memcmp(s, "PUT", 3) == 0)    return 3;
    if (len == 6 && memcmp(s, "DELETE", 6) == 0) return 4;
    return 0;
}

/*
parses one combined-format line. this is the actual work the benchmark
measures: field splitting, integer conversion, dotted-quad packing and a
path hash. deliberately written the straightforward way rather than
optimized, because it stands in for real per-task work.
*/
static inline int workload_parse(const char* line, uint32_t len, LogRecord* out) {
    memset(out, 0, sizeof(*out));

    const char* p   = line;
    const char* end = line + len;

    /* dotted quad -> packed u32 */
    uint32_t ip = 0;
    for (int octet = 0; octet < 4; octet++) {
        uint32_t v = 0;
        int digits = 0;
        while (p < end && *p >= '0' && *p <= '9') { v = v * 10 + (uint32_t)(*p++ - '0'); digits++; }
        if (!digits || v > 255) return -1;
        ip = (ip << 8) | v;
        if (octet < 3) {
            if (p >= end || *p != '.') return -1;
            p++;
        }
    }
    out->ip = ip;

    /* skip to the quoted request */
    while (p < end && *p != '"') p++;
    if (p >= end) return -1;
    p++;

    const char* method = p;
    while (p < end && *p != ' ') p++;
    if (p >= end) return -1;
    out->method_id = wl_method_id(method, (size_t)(p - method));
    p++;

    /* fnv-1a over the path */
    uint32_t h = 2166136261u;
    while (p < end && *p != ' ') {
        h ^= (uint8_t)*p++;
        h *= 16777619u;
    }
    out->path_hash = h;

    /* skip the protocol token and closing quote */
    while (p < end && *p != '"') p++;
    if (p >= end) return -1;
    p++;
    if (p < end && *p == ' ') p++;

    int status = 0;
    while (p < end && *p >= '0' && *p <= '9') status = status * 10 + (*p++ - '0');
    if (status < 100 || status > 599) return -1;
    out->status = status;

    if (p < end && *p == ' ') p++;
    uint32_t bytes = 0;
    while (p < end && *p >= '0' && *p <= '9') bytes = bytes * 10 + (uint32_t)(*p++ - '0');
    out->bytes = bytes;

    out->ok = 1;
    return 0;
}

/*
processes one task payload and folds it into the aggregate. `type` selects
how much work is done, so a mixed workload has genuinely uneven per-task
cost — log.audit walks the line a second time to normalize the user agent.
*/
static inline void workload_process(const char* type, const char* payload,
                                    uint32_t len, LogAggregate* agg) {
    LogRecord rec;
    if (workload_parse(payload, len, &rec) != 0) {
        agg->parse_failures++;
        return;
    }

    agg->lines++;
    agg->bytes_total += rec.bytes;
    agg->by_class[rec.status / 100]++;
    agg->hash_acc ^= rec.path_hash;

    if (strcmp(type, "log.parse") == 0) return;

    /* enrich: fold the client ip into a coarse bucket */
    if (strcmp(type, "log.enrich") == 0) {
        uint32_t net = rec.ip & 0xFFFFFF00u;
        agg->hash_acc = (agg->hash_acc * 31u) ^ net;
        return;
    }

    /* audit: second pass to lowercase-normalize the user agent */
    uint32_t h = 2166136261u;
    for (uint32_t i = 0; i < len; i++) {
        char ch = payload[i];
        if (ch >= 'A' && ch <= 'Z') ch = (char)(ch + 32);
        h ^= (uint8_t)ch;
        h *= 16777619u;
    }
    agg->hash_acc ^= h;
}

#endif
