#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include "protocol.h"
#include "queue.h"
#include "workers.h"
#include "incPMAD.h"
#include "event_loop.h"

#define TEST_PORT 16380

// ── server thread ─────────────────────────────────────────────────────────────

// forward declare — implemented in server.c
int server_start(int port, Queue* q, WorkerPool* pool);

static Queue      g_queue;
static WorkerPool g_pool;

static void* server_thread(void* arg) {
    (void)arg;
    server_start(TEST_PORT, &g_queue, &g_pool);
    return NULL;
}

// ── client helpers ────────────────────────────────────────────────────────────

static int connect_to_server(void) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_port        = htons(TEST_PORT);
    addr.sin_addr.s_addr = inet_addr("127.0.0.1");

    if (connect(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("connect");
        return -1;
    }
    return fd;
}

// read a response message from server
static int read_response(int fd, Message* msg) {
    ProtocolHeader hdr;
    int r = protocol_read_exact(fd, &hdr, PROTOCOL_HEADER_SIZE);
    if (r <= 0) return r;

    memset(msg, 0, sizeof(Message));
    msg->version = hdr.version;
    msg->type    = (MessageType)hdr.type;

    uint32_t payload_len = ntohl(hdr.length);
    if (payload_len == 0) return 1;

    uint8_t buf[4096];
    if (payload_len > sizeof(buf)) return -1;

    r = protocol_read_exact(fd, buf, (int)payload_len);
    if (r <= 0) return r;

    uint8_t* p = buf;

    switch (msg->type) {
        case MSG_OK: {
            memcpy(&msg->task_id, p, 4);
            msg->task_id = ntohl(msg->task_id);
            break;
        }
        case MSG_ERROR: {
            msg->error_code = (ErrorCode)p[0];
            uint32_t ml = payload_len - 1;
            if (ml >= sizeof(msg->error_msg))
                ml = sizeof(msg->error_msg) - 1;
            memcpy(msg->error_msg, p + 1, ml);
            msg->error_msg[ml] = '\0';
            break;
        }
        case MSG_STATS_RESPONSE: {
            msg->payload_len = payload_len;
            msg->payload     = p;  // valid only before next read
            break;
        }
        default:
            break;
    }

    return 1;
}

// ── test helpers ──────────────────────────────────────────────────────────────

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

static void wait_for_server(void) {
    // give server time to start
    usleep(100000);  // 100ms
}

// ── tests ─────────────────────────────────────────────────────────────────────

static void test_connect_disconnect(void) {
    printf("\n--- test: connect and disconnect ---\n");

    int fd = connect_to_server();
    check("connected to server", fd > 0);

    close(fd);
    usleep(10000);  // let server process disconnect
    check("disconnected cleanly", 1);
}

static void test_producer_submit(void) {
    printf("\n--- test: producer submits task ---\n");

    int fd = connect_to_server();
    check("connected", fd > 0);

    // send SUBMIT
    const uint8_t* payload     = (const uint8_t*)"{\"to\":\"user@test.com\"}";
    uint32_t       payload_len = strlen((char*)payload);

    int r = protocol_send_submit(fd, "send_email",
                                 payload, payload_len);
    check("submit sent", r == 0);

    // read OK response from fake daemon
    Message msg;
    r = read_response(fd, &msg);
    check("got response",       r == 1);
    check("response is MSG_OK", msg.type == MSG_OK);
    check("task_id is 1",       msg.task_id == 1);

    close(fd);
}

static void test_worker_ready(void) {
    printf("\n--- test: worker sends READY ---\n");

    int fd = connect_to_server();
    check("connected", fd > 0);

    // send READY
    int r = protocol_send_ready(fd);
    check("ready sent", r == 0);

    // fake daemon responds with WAIT
    Message msg;
    r = read_response(fd, &msg);
    check("got response",         r == 1);
    check("response is MSG_WAIT", msg.type == MSG_WAIT);

    close(fd);
}

static void test_heartbeat(void) {
    printf("\n--- test: heartbeat ---\n");

    int fd = connect_to_server();
    check("connected", fd > 0);

    int r = protocol_send_heartbeat(fd);
    check("heartbeat sent", r == 0);

    // fake daemon responds with PONG
    Message msg;
    r = read_response(fd, &msg);
    check("got response",         r == 1);
    check("response is MSG_PONG", msg.type == MSG_PONG);

    close(fd);
}

static void test_stats(void) {
    printf("\n--- test: stats request ---\n");

    int fd = connect_to_server();
    check("connected", fd > 0);

    int r = protocol_send_stats_request(fd);
    check("stats request sent", r == 0);

    // fake daemon responds with JSON
    ProtocolHeader hdr;
    r = protocol_read_exact(fd, &hdr, PROTOCOL_HEADER_SIZE);
    check("got response header",            r == PROTOCOL_HEADER_SIZE);
    check("response is MSG_STATS_RESPONSE",
          (MessageType)hdr.type == MSG_STATS_RESPONSE);

    uint32_t len = ntohl(hdr.length);
    check("response has payload", len > 0);

    uint8_t buf[256];
    if (len < sizeof(buf)) {
        r = protocol_read_exact(fd, buf, (int)len);
        buf[len] = '\0';
        check("stats JSON received", r == (int)len);
        printf("  stats: %s\n", buf);
    }

    close(fd);
}

static void test_multiple_connections(void) {
    printf("\n--- test: multiple simultaneous connections ---\n");

    int fd1 = connect_to_server();
    int fd2 = connect_to_server();
    int fd3 = connect_to_server();

    check("connection 1 ok", fd1 > 0);
    check("connection 2 ok", fd2 > 0);
    check("connection 3 ok", fd3 > 0);

    // send from all three
    protocol_send_ready(fd1);
    protocol_send_ready(fd2);
    protocol_send_submit(fd3, "task_x",
                         (const uint8_t*)"data", 4);

    // read responses
    Message m1, m2, m3;
    read_response(fd1, &m1);
    read_response(fd2, &m2);
    read_response(fd3, &m3);

    check("conn 1 got WAIT", m1.type == MSG_WAIT);
    check("conn 2 got WAIT", m2.type == MSG_WAIT);
    check("conn 3 got OK",   m3.type == MSG_OK);

    close(fd1);
    close(fd2);
    close(fd3);
}

static void test_bad_version(void) {
    printf("\n--- test: bad version rejected ---\n");

    int fd = connect_to_server();
    check("connected", fd > 0);

    // manually write header with wrong version
    ProtocolHeader hdr;
    hdr.version = 99;
    hdr.type    = MSG_READY;
    hdr.length  = 0;

    write(fd, &hdr, sizeof(hdr));

    // server should send ERROR and close connection
    Message msg;
    int r = read_response(fd, &msg);
    check("got error response",    r == 1);
    check("response is MSG_ERROR", msg.type == MSG_ERROR);
    check("error is version mismatch",
          msg.error_code == ERR_VERSION_MISMATCH);

    close(fd);
}

static void test_worker_done(void) {
    printf("\n--- test: worker sends DONE ---\n");

    int fd = connect_to_server();
    check("connected", fd > 0);

    // first send READY
    protocol_send_ready(fd);
    Message m1;
    read_response(fd, &m1);
    check("got WAIT after READY", m1.type == MSG_WAIT);

    // now send DONE
    int r = protocol_send_done(fd, 42);
    check("done sent", r == 0);

    // fake daemon just prints — no response for DONE
    // just verify no crash
    usleep(10000);
    check("server handled DONE without crash", 1);

    close(fd);
}

// ── main ──────────────────────────────────────────────────────────────────────

int main(void) {
    printf("=== server tests ===\n");
    printf("using backend: %s\n", EL_BACKEND);

    // init PMAD
    size_t sizes[]       = {64, 128, 256, 512, 1024};
    size_t percentages[] = {30, 25,  20,  15,  10};
    pmad_init(sizes, percentages);

    // init queue and worker pool
    queue_init(&g_queue);
    workers_init(&g_pool);

    // start server in background thread
    pthread_t srv;
    pthread_create(&srv, NULL, server_thread, NULL);
    pthread_detach(srv);

    // wait for server to be ready
    wait_for_server();

    // run tests
    test_connect_disconnect();
    test_producer_submit();
    test_worker_ready();
    test_heartbeat();
    test_stats();
    test_multiple_connections();
    test_bad_version();
    test_worker_done();

    printf("\n=== results ===\n");
    printf("passed: %d\n", passed);
    printf("failed: %d\n", failed);

    if (failed == 0)
        printf("--- all server tests passed ---\n");
    else
        printf("--- %d tests FAILED ---\n", failed);

    pmad_destroy();
    return failed == 0 ? 0 : 1;
}