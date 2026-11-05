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

int main(void) {
    printf("=== wire tests ===\n");

    test_task_with_payload(13);
    test_task_with_payload(PROTOCOL_MAX_PAYLOAD - 4 - 1 - 12);
    test_submit_with_payload();
    test_back_to_back_tasks();

    printf("\n=== results ===\npassed: %d\nfailed: %d\n", passed, failed);
    return failed == 0 ? 0 : 1;
}
