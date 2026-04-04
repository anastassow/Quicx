#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <stdlib.h>
#include "protocol.h"

// helpers ───────────────────────────────────────────────────────────────────

static void make_pipe(int* rfd, int* wfd) {
    int fds[2];
    pipe(fds);
    *rfd = fds[0];
    *wfd = fds[1];
}

static int roundtrip(int rfd, int wfd, Message* msg) {
    uint8_t buf[PROTOCOL_MAX_PAYLOAD];
    return protocol_read_message(rfd, buf, sizeof(buf), msg);
}

static int passed = 0;
static int failed = 0;

static void check(const char* name, int condition) {
    if (condition) {
        printf("  PASS: %s\n", name);
        passed++;
    } else {
        printf("  FAIL: %s\n", name);
        failed++;
    }
}

// tests ─────────────────────────────────────────────────────────────────────

static void test_submit(void) {
    printf("\n--- MSG_SUBMIT ---\n");
    int rfd, wfd;
    make_pipe(&rfd, &wfd);

    const char*    type        = "send_email";
    const uint8_t* payload     = (const uint8_t*)"{\"to\":\"user@gmail.com\"}";
    uint32_t       payload_len = strlen((char*)payload);

    protocol_send_submit(wfd, type, payload, payload_len);

    Message msg;
    int r = roundtrip(rfd, wfd, &msg);

    check("return value is 1",            r == 1);
    check("type is MSG_SUBMIT",           msg.type == MSG_SUBMIT);
    check("version is correct",           msg.version == PROTOCOL_VERSION);
    check("task_type matches",            strcmp(msg.task_type, type) == 0);
    check("payload_len matches",          msg.payload_len == payload_len);
    check("payload content matches",      memcmp(msg.payload, payload,
                                                  payload_len) == 0);

    close(rfd);
    close(wfd);
}

static void test_submit_no_payload(void) {
    printf("\n--- MSG_SUBMIT no payload ---\n");
    int rfd, wfd;
    make_pipe(&rfd, &wfd);

    protocol_send_submit(wfd, "ping", NULL, 0);

    Message msg;
    int r = roundtrip(rfd, wfd, &msg);

    check("return value is 1",   r == 1);
    check("type is MSG_SUBMIT",  msg.type == MSG_SUBMIT);
    check("task_type matches",   strcmp(msg.task_type, "ping") == 0);
    check("payload_len is 0",    msg.payload_len == 0);

    close(rfd);
    close(wfd);
}

static void test_ok(void) {
    printf("\n--- MSG_OK ---\n");
    int rfd, wfd;
    make_pipe(&rfd, &wfd);

    protocol_send_ok(wfd, 8821);

    Message msg;
    int r = roundtrip(rfd, wfd, &msg);

    check("return value is 1",  r == 1);
    check("type is MSG_OK",     msg.type == MSG_OK);
    check("task_id is 8821",    msg.task_id == 8821);

    close(rfd);
    close(wfd);
}

static void test_error(void) {
    printf("\n--- MSG_ERROR ---\n");
    int rfd, wfd;
    make_pipe(&rfd, &wfd);

    protocol_send_error(wfd, ERR_QUEUE_FULL, "queue is full");

    Message msg;
    int r = roundtrip(rfd, wfd, &msg);

    check("return value is 1",       r == 1);
    check("type is MSG_ERROR",       msg.type == MSG_ERROR);
    check("error_code matches",      msg.error_code == ERR_QUEUE_FULL);
    check("error_msg matches",       strcmp(msg.error_msg,
                                            "queue is full") == 0);

    close(rfd);
    close(wfd);
}

static void test_ready(void) {
    printf("\n--- MSG_READY ---\n");
    int rfd, wfd;
    make_pipe(&rfd, &wfd);

    protocol_send_ready(wfd);

    Message msg;
    int r = roundtrip(rfd, wfd, &msg);

    check("return value is 1",   r == 1);
    check("type is MSG_READY",   msg.type == MSG_READY);

    close(rfd);
    close(wfd);
}

static void test_done(void) {
    printf("\n--- MSG_DONE ---\n");
    int rfd, wfd;
    make_pipe(&rfd, &wfd);

    protocol_send_done(wfd, 42);

    Message msg;
    int r = roundtrip(rfd, wfd, &msg);

    check("return value is 1",  r == 1);
    check("type is MSG_DONE",   msg.type == MSG_DONE);
    check("task_id is 42",      msg.task_id == 42);

    close(rfd);
    close(wfd);
}

static void test_failed(void) {
    printf("\n--- MSG_FAILED ---\n");
    int rfd, wfd;
    make_pipe(&rfd, &wfd);

    protocol_send_failed(wfd, 99, "timeout");

    Message msg;
    int r = roundtrip(rfd, wfd, &msg);

    check("return value is 1",   r == 1);
    check("type is MSG_FAILED",  msg.type == MSG_FAILED);
    check("task_id is 99",       msg.task_id == 99);

    close(rfd);
    close(wfd);
}

static void test_task(void) {
    printf("\n--- MSG_TASK ---\n");
    int rfd, wfd;
    make_pipe(&rfd, &wfd);

    const char*    type        = "resize_image";
    const uint8_t* payload     = (const uint8_t*)"{\"width\":800}";
    uint32_t       payload_len = strlen((char*)payload);

    protocol_send_task(wfd, 1234, type, payload, payload_len);

    Message msg;
    int r = roundtrip(rfd, wfd, &msg);

    check("return value is 1",       r == 1);
    check("type is MSG_TASK",        msg.type == MSG_TASK);
    check("task_id is 1234",         msg.task_id == 1234);
    check("task_type matches",       strcmp(msg.task_type, type) == 0);
    check("payload_len matches",     msg.payload_len == payload_len);
    check("payload content matches", memcmp(msg.payload, payload,
                                            payload_len) == 0);

    close(rfd);
    close(wfd);
}

static void test_task_no_payload(void) {
    printf("\n--- MSG_TASK no payload ---\n");
    int rfd, wfd;
    make_pipe(&rfd, &wfd);

    protocol_send_task(wfd, 5, "heartbeat_check", NULL, 0);

    Message msg;
    int r = roundtrip(rfd, wfd, &msg);

    check("return value is 1",      r == 1);
    check("type is MSG_TASK",       msg.type == MSG_TASK);
    check("task_id is 5",           msg.task_id == 5);
    check("task_type matches",      strcmp(msg.task_type,
                                          "heartbeat_check") == 0);
    check("payload_len is 0",       msg.payload_len == 0);

    close(rfd);
    close(wfd);
}

static void test_wait(void) {
    printf("\n--- MSG_WAIT ---\n");
    int rfd, wfd;
    make_pipe(&rfd, &wfd);

    protocol_send_wait(wfd);

    Message msg;
    int r = roundtrip(rfd, wfd, &msg);

    check("return value is 1",  r == 1);
    check("type is MSG_WAIT",   msg.type == MSG_WAIT);

    close(rfd);
    close(wfd);
}

static void test_heartbeat(void) {
    printf("\n--- MSG_HEARTBEAT ---\n");
    int rfd, wfd;
    make_pipe(&rfd, &wfd);

    protocol_send_heartbeat(wfd);

    Message msg;
    int r = roundtrip(rfd, wfd, &msg);

    check("return value is 1",       r == 1);
    check("type is MSG_HEARTBEAT",   msg.type == MSG_HEARTBEAT);

    close(rfd);
    close(wfd);
}

static void test_pong(void) {
    printf("\n--- MSG_PONG ---\n");
    int rfd, wfd;
    make_pipe(&rfd, &wfd);

    protocol_send_pong(wfd);

    Message msg;
    int r = roundtrip(rfd, wfd, &msg);

    check("return value is 1",   r == 1);
    check("type is MSG_PONG",    msg.type == MSG_PONG);

    close(rfd);
    close(wfd);
}

static void test_stats_request(void) {
    printf("\n--- MSG_STATS ---\n");
    int rfd, wfd;
    make_pipe(&rfd, &wfd);

    protocol_send_stats_request(wfd);

    Message msg;
    int r = roundtrip(rfd, wfd, &msg);

    check("return value is 1",  r == 1);
    check("type is MSG_STATS",  msg.type == MSG_STATS);

    close(rfd);
    close(wfd);
}

static void test_stats_response(void) {
    printf("\n--- MSG_STATS_RESPONSE ---\n");
    int rfd, wfd;
    make_pipe(&rfd, &wfd);

    const char* json     = "{\"idle\":3,\"busy\":1,\"queued\":5}";
    uint32_t    json_len = strlen(json);

    protocol_send_stats_response(wfd,
                                 (const uint8_t*)json,
                                 json_len);

    Message msg;
    int r = roundtrip(rfd, wfd, &msg);

    check("return value is 1",           r == 1);
    check("type is MSG_STATS_RESPONSE",  msg.type == MSG_STATS_RESPONSE);
    check("payload_len matches",         msg.payload_len == json_len);
    check("payload content matches",     memcmp(msg.payload, json,
                                                json_len) == 0);

    close(rfd);
    close(wfd);
}

static void test_large_payload(void) {
    printf("\n--- large payload (512KB) ---\n");
    int rfd, wfd;
    make_pipe(&rfd, &wfd);

    uint32_t len = 32 * 1024;
    uint8_t* big = malloc(len);
    memset(big, 0xAB, len);

    protocol_send_submit(wfd, "process_image", big, len);

    Message  msg;
    uint8_t* buf = malloc(len + 64);
    int r = protocol_read_message(rfd, buf, len + 64, &msg);

    check("return value is 1",       r == 1);
    check("type is MSG_SUBMIT",      msg.type == MSG_SUBMIT);
    check("payload_len matches",     msg.payload_len == len);
    check("payload content matches", memcmp(msg.payload, big, len) == 0);

    free(big);
    free(buf);
    close(rfd);
    close(wfd);
}

static void test_bad_version(void) {
    printf("\n--- bad version rejected ---\n");
    int rfd, wfd;
    make_pipe(&rfd, &wfd);

    ProtocolHeader hdr;
    hdr.version = 99;
    hdr.type    = MSG_READY;
    hdr.length  = 0;

    write(wfd, &hdr, sizeof(hdr));

    uint8_t drain[64];
    read(rfd, drain, sizeof(drain));

    check("bad version handled without crash", 1);

    close(rfd);
    close(wfd);
}

static void test_multiple_messages(void) {
    printf("\n--- multiple messages in sequence ---\n");
    int rfd, wfd;
    make_pipe(&rfd, &wfd);

    protocol_send_ready(wfd);
    protocol_send_done(wfd, 100);
    protocol_send_failed(wfd, 200, "oops");

    uint8_t buf[1024];
    Message msg;

    int r1 = protocol_read_message(rfd, buf, sizeof(buf), &msg);
    check("msg 1 — return value is 1",  r1 == 1);
    check("msg 1 — type is MSG_READY",  msg.type == MSG_READY);

    int r2 = protocol_read_message(rfd, buf, sizeof(buf), &msg);
    check("msg 2 — return value is 1",  r2 == 1);
    check("msg 2 — type is MSG_DONE",   msg.type == MSG_DONE);
    check("msg 2 — task_id is 100",     msg.task_id == 100);

    int r3 = protocol_read_message(rfd, buf, sizeof(buf), &msg);
    check("msg 3 — return value is 1",   r3 == 1);
    check("msg 3 — type is MSG_FAILED",  msg.type == MSG_FAILED);
    check("msg 3 — task_id is 200",      msg.task_id == 200);

    close(rfd);
    close(wfd);
}

static void test_disconnect(void) {
    printf("\n--- disconnect detected ---\n");
    int rfd, wfd;
    make_pipe(&rfd, &wfd);

    close(wfd);

    uint8_t buf[64];
    Message msg;
    int r = protocol_read_message(rfd, buf, sizeof(buf), &msg);

    check("disconnect returns 0", r == 0);

    close(rfd);
}


int main(void) {
    printf("=== protocol tests ===\n");

    test_submit();
    test_submit_no_payload();
    test_ok();
    test_error();
    test_ready();
    test_done();
    test_failed();
    test_task();
    test_task_no_payload();
    test_wait();
    test_heartbeat();
    test_pong();
    test_stats_request();
    test_stats_response();
    test_large_payload();
    test_bad_version();
    test_multiple_messages();
    test_disconnect();

    printf("\n=== results ===\n");
    printf("passed: %d\n", passed);
    printf("failed: %d\n", failed);

    if (failed == 0)
        printf("--- all protocol tests passed ---\n");
    else
        printf("--- %d tests FAILED ---\n", failed);

    return failed == 0 ? 0 : 1;
}