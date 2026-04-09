#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include "protocol.h"
#include "queue.h"
#include "workers.h"
#include "incPMAD.h"
#include "event_loop.h"

#define TEST_PORT 16381

// ── server ────────────────────────────────────────────────────────────────────

int server_start(int port, Queue* q, WorkerPool* pool);

static Queue      g_queue;
static WorkerPool g_pool;

static void* server_thread(void* arg) {
    (void)arg;
    server_start(TEST_PORT, &g_queue, &g_pool);
    return NULL;
}

// ── helpers ───────────────────────────────────────────────────────────────────

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

static void set_timeout(int fd, int seconds) {
    struct timeval tv;
    tv.tv_sec  = seconds;
    tv.tv_usec = 0;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
}

// read one complete message — returns 1 ok, 0 disconnect, -1 error
static int read_msg(int fd, Message* msg) {
    ProtocolHeader hdr;
    int r = protocol_read_exact(fd, &hdr, PROTOCOL_HEADER_SIZE);
    if (r <= 0) return r;

    memset(msg, 0, sizeof(Message));
    msg->version = hdr.version;
    msg->type    = (MessageType)hdr.type;

    uint32_t payload_len = ntohl(hdr.length);
    if (payload_len == 0) return 1;

    static uint8_t buf[PROTOCOL_MAX_PAYLOAD];
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
        case MSG_TASK: {
            if (payload_len < 5) return -1;
            memcpy(&msg->task_id, p, 4);
            msg->task_id     = ntohl(msg->task_id);
            uint8_t type_len = p[4];
            if (type_len >= PROTOCOL_MAX_TYPE_LEN) return -1;
            memcpy(msg->task_type, p + 5, type_len);
            msg->task_type[type_len] = '\0';
            msg->payload     = p + 5 + type_len;
            msg->payload_len = payload_len - 5 - type_len;
            break;
        }
        case MSG_STATS_RESPONSE: {
            msg->payload     = p;
            msg->payload_len = payload_len;
            break;
        }
        default:
            break;
    }

    return 1;
}

// ── test state ────────────────────────────────────────────────────────────────

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

// ── tests ─────────────────────────────────────────────────────────────────────

// one producer, one worker, one task
static void test_single_task(void) {
    printf("\n--- test: single task end to end ---\n");

    int worker_fd   = connect_to_server();
    int producer_fd = connect_to_server();
    set_timeout(worker_fd,   2);
    set_timeout(producer_fd, 2);

    // worker registers
    protocol_send_ready(worker_fd);
    usleep(20000);

    // producer submits
    const char*    task_type   = "send_email";
    const uint8_t* payload     = (const uint8_t*)"{\"to\":\"a@b.com\"}";
    uint32_t       payload_len = strlen((char*)payload);

    protocol_send_submit(producer_fd, task_type, payload, payload_len);

    // producer gets OK
    Message ok;
    int r = read_msg(producer_fd, &ok);
    check("producer got OK",      r == 1 && ok.type == MSG_OK);
    check("task_id is valid",     ok.task_id > 0);

    // worker gets TASK
    Message task;
    r = read_msg(worker_fd, &task);
    check("worker got TASK",         r == 1 && task.type == MSG_TASK);
    check("task_id matches",         task.task_id == ok.task_id);
    check("task_type is send_email", strcmp(task.task_type, "send_email") == 0);
    check("payload matches",         task.payload_len == payload_len &&
                                     memcmp(task.payload, payload,
                                            payload_len) == 0);

    // worker executes and sends DONE
    protocol_send_done(worker_fd, task.task_id);
    usleep(20000);
    check("worker sent DONE", 1);

    close(worker_fd);
    close(producer_fd);
}

// task submitted before worker connects — must queue and dispatch on READY
static void test_task_queued_before_worker(void) {
    printf("\n--- test: task queues then worker connects ---\n");

    int producer_fd = connect_to_server();
    set_timeout(producer_fd, 2);

    // submit with no workers
    protocol_send_submit(producer_fd, "resize_image",
                         (const uint8_t*)"{\"w\":800}", 9);

    Message ok;
    int r = read_msg(producer_fd, &ok);
    check("producer got OK",       r == 1 && ok.type == MSG_OK);
    check("task queued",           ok.task_id > 0);

    usleep(20000);

    // now worker connects
    int worker_fd = connect_to_server();
    set_timeout(worker_fd, 2);
    protocol_send_ready(worker_fd);

    // worker should get the queued task immediately
    Message task;
    r = read_msg(worker_fd, &task);
    check("worker got queued task",  r == 1 && task.type == MSG_TASK);
    check("task_type matches",       strcmp(task.task_type,
                                           "resize_image") == 0);
    check("task_id matches",         task.task_id == ok.task_id);

    protocol_send_done(worker_fd, task.task_id);
    usleep(20000);

    close(producer_fd);
    close(worker_fd);
}

// two workers, two tasks — each worker gets one task
static void test_two_workers_two_tasks(void) {
    printf("\n--- test: two workers two tasks ---\n");

    int w1 = connect_to_server();
    int w2 = connect_to_server();
    int p  = connect_to_server();
    set_timeout(w1, 2);
    set_timeout(w2, 2);
    set_timeout(p,  2);

    // both workers register
    protocol_send_ready(w1);
    protocol_send_ready(w2);
    usleep(20000);

    // submit two tasks
    protocol_send_submit(p, "task_a", (const uint8_t*)"foo", 3);
    Message ok1;
    read_msg(p, &ok1);
    check("task_a accepted", ok1.type == MSG_OK);

    protocol_send_submit(p, "task_b", (const uint8_t*)"bar", 3);
    Message ok2;
    read_msg(p, &ok2);
    check("task_b accepted", ok2.type == MSG_OK);

    // both workers should get a task
    Message t1, t2;
    int r1 = read_msg(w1, &t1);
    int r2 = read_msg(w2, &t2);

    check("worker 1 got TASK", r1 == 1 && t1.type == MSG_TASK);
    check("worker 2 got TASK", r2 == 1 && t2.type == MSG_TASK);
    check("tasks are different", t1.task_id != t2.task_id);

    // both workers finish
    protocol_send_done(w1, t1.task_id);
    protocol_send_done(w2, t2.task_id);
    usleep(20000);

    close(w1);
    close(w2);
    close(p);
}

// worker finishes task then gets another one immediately
static void test_worker_gets_next_task_after_done(void) {
    printf("\n--- test: worker gets next task after DONE ---\n");

    int w = connect_to_server();
    int p = connect_to_server();
    set_timeout(w, 2);
    set_timeout(p, 2);

    protocol_send_ready(w);
    usleep(20000);

    // submit first task
    protocol_send_submit(p, "job_1", (const uint8_t*)"a", 1);
    Message ok1;
    read_msg(p, &ok1);
    check("job_1 accepted", ok1.type == MSG_OK);

    // worker gets first task
    Message t1;
    read_msg(w, &t1);
    check("worker got job_1", t1.type == MSG_TASK &&
                              strcmp(t1.task_type, "job_1") == 0);

    // submit second task while worker is busy
    protocol_send_submit(p, "job_2", (const uint8_t*)"b", 1);
    Message ok2;
    read_msg(p, &ok2);
    check("job_2 accepted", ok2.type == MSG_OK);

    // worker finishes first — should immediately get second
    protocol_send_done(w, t1.task_id);

    Message t2;
    int r = read_msg(w, &t2);
    check("worker got job_2 after DONE", r == 1 && t2.type == MSG_TASK &&
                                         strcmp(t2.task_type, "job_2") == 0);

    protocol_send_done(w, t2.task_id);
    usleep(20000);

    close(w);
    close(p);
}

// heartbeat keeps connection alive
static void test_heartbeat(void) {
    printf("\n--- test: heartbeat pong ---\n");

    int fd = connect_to_server();
    set_timeout(fd, 2);

    protocol_send_heartbeat(fd);

    Message msg;
    int r = read_msg(fd, &msg);
    check("got PONG", r == 1 && msg.type == MSG_PONG);

    close(fd);
}

// stats reflect real queue and pool state
static void test_stats(void) {
    printf("\n--- test: stats reflect real state ---\n");

    int w = connect_to_server();
    int p = connect_to_server();
    int s = connect_to_server();
    set_timeout(w, 2);
    set_timeout(p, 2);
    set_timeout(s, 2);

    // register worker — 1 idle
    protocol_send_ready(w);
    usleep(20000);

    // check stats — 1 idle 0 busy 0 queued
    protocol_send_stats_request(s);
    Message stats1;
    read_msg(s, &stats1);
    check("stats response received", stats1.type == MSG_STATS_RESPONSE);
    if (stats1.payload_len > 0) {
        char buf[256];
        uint32_t len = stats1.payload_len < 255 ? stats1.payload_len : 255;
        memcpy(buf, stats1.payload, len);
        buf[len] = '\0';
        printf("  stats: %s\n", buf);
        check("stats has content", stats1.payload_len > 0);
    }

    // submit task — worker becomes busy
    protocol_send_submit(p, "heavy_job", (const uint8_t*)"x", 1);
    Message ok;
    read_msg(p, &ok);
    usleep(20000);

    // check stats — 0 idle 1 busy 0 queued
    protocol_send_stats_request(s);
    Message stats2;
    read_msg(s, &stats2);
    check("stats updated after task", stats2.type == MSG_STATS_RESPONSE);
    if (stats2.payload_len > 0) {
        char buf[256];
        uint32_t len = stats2.payload_len < 255 ? stats2.payload_len : 255;
        memcpy(buf, stats2.payload, len);
        buf[len] = '\0';
        printf("  stats: %s\n", buf);
    }

    // worker finishes
    Message task;
    read_msg(w, &task);
    protocol_send_done(w, task.task_id);
    usleep(20000);

    close(w);
    close(p);
    close(s);
}

// worker disconnects — pool cleans up
static void test_worker_disconnect(void) {
    printf("\n--- test: worker disconnect cleanup ---\n");

    int w = connect_to_server();
    set_timeout(w, 2);

    protocol_send_ready(w);
    usleep(20000);

    // disconnect worker
    close(w);
    usleep(20000);

    // new worker connects — pool should be clean
    int w2 = connect_to_server();
    set_timeout(w2, 2);
    protocol_send_ready(w2);
    usleep(20000);
    check("new worker registered after disconnect", 1);

    // submit task — should go to new worker
    int p = connect_to_server();
    set_timeout(p, 2);
    protocol_send_submit(p, "after_disconnect",
                         (const uint8_t*)"x", 1);
    Message ok;
    read_msg(p, &ok);
    check("task accepted after worker reconnect", ok.type == MSG_OK);

    Message task;
    int r = read_msg(w2, &task);
    check("new worker got task", r == 1 && task.type == MSG_TASK);

    protocol_send_done(w2, task.task_id);
    usleep(20000);

    close(w2);
    close(p);
}

// ── main ──────────────────────────────────────────────────────────────────────

int main(void) {
    printf("=== daemon integration tests ===\n");
    printf("backend: %s\n", EL_BACKEND);

    size_t sizes[]       = {32, 64, 128, 256, 512, 1024};
    size_t percentages[] = {10, 25, 25,  20,  12,  8};
    pmad_init(sizes, 6, percentages, 1024 * 1024);

    queue_init(&g_queue);
    workers_init(&g_pool);

    pthread_t srv;
    pthread_create(&srv, NULL, server_thread, NULL);
    pthread_detach(srv);

    usleep(100000);  // wait for server

    test_single_task();
    test_task_queued_before_worker();
    test_two_workers_two_tasks();
    test_worker_gets_next_task_after_done();
    test_heartbeat();
    test_stats();
    test_worker_disconnect();

    printf("\n=== results ===\n");
    printf("passed: %d\n", passed);
    printf("failed: %d\n", failed);

    if (failed == 0)
        printf("--- all daemon tests passed ---\n");
    else
        printf("--- %d tests FAILED ---\n", failed);

    pmad_destroy();
    return failed == 0 ? 0 : 1;
}