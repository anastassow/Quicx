/*
fill — submits tasks with no workers attached until the daemon refuses,
then submits one more to see whether the connection is still in sync.

  fill --port P --size N        fixed payload size
  fill --port P --nginx         real nginx log lines from workload.h
*/
#include "bench_common.h"
#include "workload.h"

static int read_reply(int fd, uint8_t* type, uint8_t* body, uint32_t* len) {
    for (;;) {
        uint8_t hdr[PROTO_HEADER_SIZE];
        if (read_exact(fd, hdr, PROTO_HEADER_SIZE) <= 0) return -1;
        uint32_t n;
        memcpy(&n, hdr + 2, 4);
        n = ntohl(n);
        if (n > PROTO_MAX_PAYLOAD + 64) return -1;
        if (n && read_exact(fd, body, (int)n) <= 0) return -1;
        if (hdr[1] == MSG_DONE || hdr[1] == MSG_FAILED) continue;   /* completions, not replies */
        *type = hdr[1];
        *len  = n;
        return 0;
    }
}

int main(int argc, char** argv) {
    int port = 16381, size = 100, nginx = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--port") && i + 1 < argc) port = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--size") && i + 1 < argc) size = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--nginx")) nginx = 1;
    }

    int fd = bench_connect("127.0.0.1", port);
    if (fd < 0) { fprintf(stderr, "connect failed\n"); return 1; }

    Rng rng;
    rng_seed(&rng, 7);
    uint8_t  payload[PROTO_MAX_PAYLOAD], body[PROTO_MAX_PAYLOAD + 64], reply[PROTO_MAX_PAYLOAD + 64];
    uint64_t accepted = 0, bytes = 0;
    uint32_t pmin = UINT32_MAX, pmax = 0;
    char     err[128] = "";

    for (uint64_t i = 0; i < 500000; i++) {
        uint32_t plen;
        const char* type = "fill";
        if (nginx) {
            plen = workload_gen_line((char*)payload, sizeof(payload), &rng);
            type = WL_TYPES[i % WL_TYPE_COUNT];
        } else {
            plen = (uint32_t)size;
            memset(payload, 'x', plen);
        }
        uint32_t blen = encode_submit(body, type, payload, plen);
        if (frame_write(fd, MSG_SUBMIT, body, blen) < 0) { snprintf(err, sizeof(err), "write failed"); break; }

        uint8_t t; uint32_t rlen;
        if (read_reply(fd, &t, reply, &rlen) < 0) { snprintf(err, sizeof(err), "connection closed"); break; }
        if (t == MSG_OK) {
            accepted++; bytes += plen;
            if (plen < pmin) pmin = plen;
            if (plen > pmax) pmax = plen;
            continue;
        }
        snprintf(err, sizeof(err), "%.*s", rlen > 1 ? (int)rlen - 1 : 0, (char*)reply + 1);
        break;
    }

    /* one more submit: an in-sync connection answers with a proper frame */
    const char* after = "connection closed";
    uint32_t blen = encode_submit(body, "fill", (const uint8_t*)"probe", 5);
    if (frame_write(fd, MSG_SUBMIT, body, blen) == 0) {
        uint8_t t; uint32_t rlen;
        if (read_reply(fd, &t, reply, &rlen) == 0) {
            static char buf[96];
            snprintf(buf, sizeof(buf), "%s \"%.*s\"", t == MSG_OK ? "OK" : t == MSG_ERROR ? "ERROR" : "other",
                     (t == MSG_ERROR && rlen > 1) ? (int)rlen - 1 : 0, (char*)reply + 1);
            after = buf;
        }
    }

    printf("accepted=%llu refused_with=\"%s\" next_submit=%s payload_min=%u payload_max=%u payload_avg=%.0f\n",
           (unsigned long long)accepted, err, after,
           accepted ? pmin : 0, pmax, accepted ? (double)bytes / accepted : 0.0);
    close(fd);
    return 0;
}
