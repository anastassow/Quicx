/*
wire-level tests for the send side of the protocol: what bytes
actually land on the fd. regression for send_frame_v overflowing its
iovec array when a frame has a header plus 3 parts (submit and task
with a payload) — the payload never reached the wire, or the stack
protector aborted the process.
*/
#include "platform.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <stdint.h>
#include <fcntl.h>
#include <signal.h>
#include <time.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <arpa/inet.h>

#include "protocol.h"

static int passed = 0;
static int failed = 0;

static void check(const char* name, int condition) {
    if (condition) { printf("  PASS: %s\n", name); passed++; }
    else           { printf("  FAIL: %s\n", name); failed++; }
}

/* reads everything written to rfd until the writer closes; returns byte count */
static int drain(int rfd, uint8_t* buf, int cap) {
    int total = 0;
    while (total < cap) {
        int r = (int)read(rfd, buf + total, cap - total);
        if (r <= 0) break;
        total += r;
    }
    return total;
}

static uint32_t header_len(const uint8_t* buf) {
    uint32_t len;
    memcpy(&len, buf + 2, 4);
    return ntohl(len);
}

static void test_task_with_payload(uint32_t payload_len) {
    printf("\n--- MSG_TASK on the wire, %u B payload ---\n", payload_len);
    int fds[2];
    pipe(fds);

    const char* type = "resize_image";
    uint8_t     payload[PROTOCOL_MAX_PAYLOAD];
    for (uint32_t i = 0; i < payload_len; i++) payload[i] = (uint8_t)(i * 7 + 1);

    int rc = protocol_send_task(fds[1], 1234, type, payload, payload_len);
    close(fds[1]);

    uint8_t  buf[PROTOCOL_HEADER_SIZE + PROTOCOL_MAX_PAYLOAD + 64];
    int      n        = drain(fds[0], buf, sizeof(buf));
    uint32_t type_len = (uint32_t)strlen(type);
    uint32_t body_len = 4 + 1 + type_len + payload_len;
    uint32_t net_id;
    memcpy(&net_id, buf + PROTOCOL_HEADER_SIZE, 4);

    check("send returns 0",                   rc == 0);
    check("header version",                   buf[0] == PROTOCOL_VERSION);
    check("header type is MSG_TASK",          buf[1] == MSG_TASK);
    check("header length = id+len+type+payload", header_len(buf) == body_len);
    check("every promised byte is on the wire", n == (int)(PROTOCOL_HEADER_SIZE + body_len));
    check("task id",                          ntohl(net_id) == 1234);
    check("type length byte",                 buf[PROTOCOL_HEADER_SIZE + 4] == type_len);
    check("type bytes",                       memcmp(buf + PROTOCOL_HEADER_SIZE + 5, type, type_len) == 0);
    check("payload bytes",                    memcmp(buf + PROTOCOL_HEADER_SIZE + 5 + type_len,
                                                     payload, payload_len) == 0);
    close(fds[0]);
}

static void test_submit_with_payload(void) {
    printf("\n--- MSG_SUBMIT on the wire ---\n");
    int fds[2];
    pipe(fds);

    const char*    type    = "send_email";
    const uint8_t* payload = (const uint8_t*)"{\"to\":\"user@example.com\"}";
    uint32_t       plen    = (uint32_t)strlen((const char*)payload);

    int rc = protocol_send_submit(fds[1], type, payload, plen);
    close(fds[1]);

    uint8_t  buf[256];
    int      n        = drain(fds[0], buf, sizeof(buf));
    uint32_t type_len = (uint32_t)strlen(type);
    uint32_t body_len = 1 + type_len + plen;

    check("send returns 0",                     rc == 0);
    check("header type is MSG_SUBMIT",          buf[1] == MSG_SUBMIT);
    check("header length = len+type+payload",   header_len(buf) == body_len);
    check("every promised byte is on the wire", n == (int)(PROTOCOL_HEADER_SIZE + body_len));
    check("type length byte",                   buf[PROTOCOL_HEADER_SIZE] == type_len);
    check("type bytes",                         memcmp(buf + PROTOCOL_HEADER_SIZE + 1, type, type_len) == 0);
    check("payload bytes",                      memcmp(buf + PROTOCOL_HEADER_SIZE + 1 + type_len,
                                                       payload, plen) == 0);
    close(fds[0]);
}

static void test_back_to_back_tasks(void) {
    printf("\n--- two MSG_TASK frames back to back ---\n");
    int fds[2];
    pipe(fds);

    protocol_send_task(fds[1], 1, "a", (const uint8_t*)"first",  5);
    protocol_send_task(fds[1], 2, "b", (const uint8_t*)"second", 6);
    close(fds[1]);

    uint8_t buf[128];
    int     n      = drain(fds[0], buf, sizeof(buf));
    int     first  = PROTOCOL_HEADER_SIZE + (int)header_len(buf);
    uint8_t* next  = buf + first;

    check("total bytes = both frames",   n == first + PROTOCOL_HEADER_SIZE + (int)header_len(next));
    check("second frame starts aligned", next[0] == PROTOCOL_VERSION && next[1] == MSG_TASK);
    check("second payload intact",       memcmp(next + PROTOCOL_HEADER_SIZE + 6, "second", 6) == 0);
    close(fds[0]);
}

static void sleep_ms(int ms) {
    struct timespec ts = { ms / 1000, (long)(ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
}

static double now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}

/*
The daemon's sockets are non-blocking. A frame split across two TCP
segments made read() fail with EAGAIN halfway through, and the daemon
closed the connection (seen with pipelined submits through Docker's
network). The rest of the frame must be waited for.
*/
static void test_read_split_frame(void) {
    printf("\n--- a frame that arrives in two pieces, 80 ms apart ---\n");
    int sv[2];
    socketpair(AF_UNIX, SOCK_STREAM, 0, sv);
    fcntl(sv[0], F_SETFL, fcntl(sv[0], F_GETFL, 0) | O_NONBLOCK);

    uint8_t frame[64];
    for (int i = 0; i < 64; i++) frame[i] = (uint8_t)i;

    pid_t pid = fork();
    if (pid == 0) {
        close(sv[0]);
        if (write(sv[1], frame, 5) != 5) _exit(1);
        sleep_ms(80);
        if (write(sv[1], frame + 5, 59) != 59) _exit(1);
        sleep_ms(200);
        _exit(0);
    }
    close(sv[1]);

    uint8_t got[64] = {0};
    int r = protocol_read_exact(sv[0], got, 64);
    waitpid(pid, NULL, 0);

    check("all 64 bytes read across the gap", r == 64);
    check("bytes intact and in order",        memcmp(got, frame, 64) == 0);
    close(sv[0]);
}

/* a peer that stops in the middle of a frame is dropped, not waited on forever */
static void test_read_stalled_peer(void) {
    printf("\n--- a peer that stops halfway through a frame ---\n");
    int sv[2];
    socketpair(AF_UNIX, SOCK_STREAM, 0, sv);
    fcntl(sv[0], F_SETFL, fcntl(sv[0], F_GETFL, 0) | O_NONBLOCK);

    if (write(sv[1], "abc", 3) != 3) { check("setup write", 0); return; }

    uint8_t got[16];
    double  t0 = now_ms();
    int     r  = protocol_read_exact(sv[0], got, 16);
    double  waited = now_ms() - t0;

    check("read gives up with an error", r < 0);
    check("after about the timeout",     waited >= PROTOCOL_IO_TIMEOUT_MS * 0.9 && waited < PROTOCOL_IO_TIMEOUT_MS * 3);
    close(sv[0]);
    close(sv[1]);
}

/* replies keep their framing when the peer's receive buffer is full for a while */
static void test_write_when_buffer_full(void) {
    printf("\n--- writing while the peer is not reading yet ---\n");
    int sv[2];
    socketpair(AF_UNIX, SOCK_STREAM, 0, sv);
    int small = 4096;
    setsockopt(sv[1], SOL_SOCKET, SO_SNDBUF, &small, sizeof(small));
    setsockopt(sv[0], SOL_SOCKET, SO_RCVBUF, &small, sizeof(small));
    fcntl(sv[1], F_SETFL, fcntl(sv[1], F_GETFL, 0) | O_NONBLOCK);

    /* fill the pipe until the kernel says it would block */
    uint8_t junk[512];
    memset(junk, 0xAB, sizeof(junk));
    int filled = 0;
    for (;;) {
        ssize_t w = write(sv[1], junk, sizeof(junk));
        if (w <= 0) break;
        filled += (int)w;
    }

    pid_t pid = fork();
    if (pid == 0) {
        close(sv[1]);
        sleep_ms(80);   /* the reader shows up late */
        uint8_t buf[1 << 16];
        int total = 0;
        for (;;) {
            ssize_t r = read(sv[0], buf + total, sizeof(buf) - (size_t)total);
            if (r <= 0) break;
            total += (int)r;
        }
        /* the frame sits right after the junk: header + 4-byte id */
        int ok = total == filled + PROTOCOL_HEADER_SIZE + 4
              && buf[filled] == PROTOCOL_VERSION && buf[filled + 1] == MSG_OK;
        _exit(ok ? 0 : 2);
    }
    close(sv[0]);

    int r = protocol_send_ok(sv[1], 42);
    close(sv[1]);
    int status = 0;
    waitpid(pid, &status, 0);

    check("send waits instead of failing", r == 0);
    check("peer gets the whole frame",     WIFEXITED(status) && WEXITSTATUS(status) == 0);
}

int main(void) {
    printf("=== wire tests ===\n");
    signal(SIGPIPE, SIG_IGN);

    test_task_with_payload(13);
    test_task_with_payload(PROTOCOL_MAX_PAYLOAD - 4 - 1 - 12);
    test_submit_with_payload();
    test_back_to_back_tasks();
    test_read_split_frame();
    test_read_stalled_peer();
    test_write_when_buffer_full();

    printf("\n=== results ===\npassed: %d\nfailed: %d\n", passed, failed);
    return failed == 0 ? 0 : 1;
}
