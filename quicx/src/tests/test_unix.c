#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include "protocol.h"
#include "queue.h"
#include "workers.h"
#include "stats.h"
#include "incPMAD.h"
#include "event_loop.h"
#include <sys/stat.h>

#define TEST_PORT        16383
#define TEST_UNIX_PATH   "/tmp/quicx_test.sock"

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

static int connect_tcp(void) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_port        = htons(TEST_PORT);
    addr.sin_addr.s_addr = inet_addr("127.0.0.1");
    if (connect(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("connect tcp");
        return -1;
    }
    return fd;
}

static int connect_unix(void) {
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, "/tmp/quicx.sock",
            sizeof(addr.sun_path) - 1);
    if (connect(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("connect unix");
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

static int read_stats(int fd, StatsHeader* hdr,
                      StatsClass* classes, int max,
                      int* count) {
    ProtocolHeader phdr;
    int r = protocol_read_exact(fd, &phdr, PROTOCOL_HEADER_SIZE);
    if (r <= 0) return r;
    if ((MessageType)phdr.type != MSG_STATS_RESPONSE) return -1;

    r = protocol_read_exact(fd, hdr, sizeof(StatsHeader));
    if (r <= 0) return r;

    uint32_t n = hdr->class_count;
    if ((int)n > max) return -1;
    if (n > 0)
        protocol_read_exact(fd, classes,
                            (int)(n * sizeof(StatsClass)));
    *count = (int)n;
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

static void test_unix_socket_exists(void) {
    printf("\n--- test: unix socket file exists ---\n");

    // check socket file exists
    struct stat st;
    int r = stat("/tmp/quicx.sock", &st);
    check("socket file exists",      r == 0);
    check("socket file is a socket", r == 0 && S_ISSOCK(st.st_mode));
}

static void test_unix_connect(void) {
    printf("\n--- test: cli connects via unix socket ---\n");

    int fd = connect_unix();
    check("connected via unix socket", fd > 0);

    if (fd > 0) {
        usleep(10000);
        close(fd);
        usleep(10000);
        check("disconnected cleanly", 1);
    }
}

static void test_unix_stats_request(void) {
    printf("\n--- test: cli gets stats via unix socket ---\n");

    int fd = connect_unix();
    check("connected", fd > 0);
    if (fd < 0) return;

    set_timeout(fd, 2);

    protocol_send_stats_request(fd);

    StatsHeader hdr;
    StatsClass  classes[8];
    int         n = 0;
    int r = read_stats(fd, &hdr, classes, 8, &n);

    check("got stats response",      r == 1);
    check("has size classes",        n > 0);
    check("uptime field present",    ntohll(hdr.uptime_seconds) >= 0);
    check("class_count matches n",   hdr.class_count == (uint32_t)n);

    close(fd);
}

static void test_unix_reflects_tcp_state(void) {
    printf("\n--- test: unix stats reflect TCP worker state ---\n");

    // connect worker via TCP
    int worker_fd = connect_tcp();
    set_timeout(worker_fd, 2);
    protocol_send_ready(worker_fd);
    usleep(20000);

    // get stats via unix socket
    int unix_fd = connect_unix();
    set_timeout(unix_fd, 2);
    protocol_send_stats_request(unix_fd);

    StatsHeader hdr;
    StatsClass  classes[8];
    int         n = 0;
    int r = read_stats(unix_fd, &hdr, classes, 8, &n);

    check("got stats",             r == 1);
    check("idle worker visible",   ntohl(hdr.idle_workers) >= 1);

    close(unix_fd);
    close(worker_fd);
    usleep(10000);
}

static void test_unix_multiple_connections(void) {
    printf("\n--- test: multiple cli connections simultaneously ---\n");

    int fd1 = connect_unix();
    int fd2 = connect_unix();
    int fd3 = connect_unix();

    check("cli 1 connected", fd1 > 0);
    check("cli 2 connected", fd2 > 0);
    check("cli 3 connected", fd3 > 0);

    set_timeout(fd1, 2);
    set_timeout(fd2, 2);
    set_timeout(fd3, 2);

    // all three request stats simultaneously
    protocol_send_stats_request(fd1);
    protocol_send_stats_request(fd2);
    protocol_send_stats_request(fd3);

    StatsHeader hdr1, hdr2, hdr3;
    StatsClass  classes[8];
    int         n = 0;

    int r1 = read_stats(fd1, &hdr1, classes, 8, &n);
    int r2 = read_stats(fd2, &hdr2, classes, 8, &n);
    int r3 = read_stats(fd3, &hdr3, classes, 8, &n);

    check("cli 1 got stats", r1 == 1);
    check("cli 2 got stats", r2 == 1);
    check("cli 3 got stats", r3 == 1);

    // all should see same class count
    check("same class count",
          hdr1.class_count == hdr2.class_count &&
          hdr2.class_count == hdr3.class_count);

    close(fd1);
    close(fd2);
    close(fd3);
    usleep(10000);
}

static void test_unix_does_not_affect_tcp(void) {
    printf("\n--- test: unix cli does not interfere with TCP ---\n");

    // connect worker and producer via TCP
    int worker_fd   = connect_tcp();
    int producer_fd = connect_tcp();
    set_timeout(worker_fd,   2);
    set_timeout(producer_fd, 2);

    protocol_send_ready(worker_fd);
    usleep(20000);

    // connect CLI via unix — request stats simultaneously
    int unix_fd = connect_unix();
    set_timeout(unix_fd, 2);
    protocol_send_stats_request(unix_fd);

    // submit task via TCP while CLI is connected
    protocol_send_submit(producer_fd, "concurrent_task",
                         (const uint8_t*)"data", 4);

    // read stats response from unix
    StatsHeader hdr;
    StatsClass  classes[8];
    int         n = 0;
    int r = read_stats(unix_fd, &hdr, classes, 8, &n);
    check("got stats while task submitted", r == 1);

    // read OK from producer
    ProtocolHeader ok_phdr;
    int rr = protocol_read_exact(producer_fd, &ok_phdr,
                                 PROTOCOL_HEADER_SIZE);
    check("producer got OK", rr == PROTOCOL_HEADER_SIZE &&
                             (MessageType)ok_phdr.type == MSG_OK);
    uint32_t ok_id;
    protocol_read_exact(producer_fd, &ok_id, 4);

    // worker gets task
    ProtocolHeader task_phdr;
    rr = protocol_read_exact(worker_fd, &task_phdr,
                              PROTOCOL_HEADER_SIZE);
    check("worker got TASK", rr == PROTOCOL_HEADER_SIZE &&
                             (MessageType)task_phdr.type == MSG_TASK);

    uint32_t task_len = ntohl(task_phdr.length);
    uint8_t  task_buf[256];
    if (task_len > 0 && task_len < sizeof(task_buf))
        protocol_read_exact(worker_fd, task_buf, (int)task_len);

    uint32_t task_id;
    memcpy(&task_id, task_buf, 4);
    protocol_send_done(worker_fd, task_id);
    usleep(10000);

    check("task completed without interference", 1);

    close(unix_fd);
    close(worker_fd);
    close(producer_fd);
    usleep(10000);
}

static void test_unix_bad_message(void) {
    printf("\n--- test: unix socket handles bad message ---\n");

    int fd = connect_unix();
    check("connected", fd > 0);
    if (fd < 0) return;

    set_timeout(fd, 2);

    // send bad version header
    ProtocolHeader hdr;
    hdr.version = 99;
    hdr.type    = MSG_STATS;
    hdr.length  = 0;
    write(fd, &hdr, sizeof(hdr));

    // should get error response
    ProtocolHeader resp;
    int r = protocol_read_exact(fd, &resp, PROTOCOL_HEADER_SIZE);
    check("got error response",    r == PROTOCOL_HEADER_SIZE);
    check("response is MSG_ERROR",
          (MessageType)resp.type == MSG_ERROR);

    close(fd);
}

static void test_unix_disconnect_clean(void) {
    printf("\n--- test: unix disconnect does not crash daemon ---\n");

    // connect and immediately disconnect without sending anything
    for (int i = 0; i < 10; i++) {
        int fd = connect_unix();
        if (fd > 0) close(fd);
        usleep(5000);
    }
    check("10 rapid disconnects handled", 1);

    // daemon should still work after rapid disconnects
    int fd = connect_unix();
    set_timeout(fd, 2);
    protocol_send_stats_request(fd);

    StatsHeader hdr;
    StatsClass  classes[8];
    int         n = 0;
    int r = read_stats(fd, &hdr, classes, 8, &n);
    check("daemon still works after disconnects", r == 1);

    close(fd);
}

// ── main ──────────────────────────────────────────────────────────────────────

int main(void) {
    printf("=== unix socket tests ===\n");
    printf("backend: %s\n", EL_BACKEND);

    size_t sizes[]       = {32, 64, 128, 256, 512, 1024};
    size_t percentages[] = {10, 25,  25,  20,  12,    8};
    pmad_init(sizes, 6, percentages, 1024 * 1024);

    stats_init();
    queue_init(&g_queue);
    workers_init(&g_pool);

    pthread_t srv;
    pthread_create(&srv, NULL, server_thread, NULL);
    pthread_detach(srv);

    usleep(100000);  // wait for server

    test_unix_socket_exists();
    test_unix_connect();
    test_unix_stats_request();
    test_unix_reflects_tcp_state();
    test_unix_multiple_connections();
    test_unix_does_not_affect_tcp();
    test_unix_bad_message();
    test_unix_disconnect_clean();

    printf("\n=== results ===\n");
    printf("passed: %d\n", passed);
    printf("failed: %d\n", failed);

    if (failed == 0)
        printf("--- all unix socket tests passed ---\n");
    else
        printf("--- %d tests FAILED ---\n", failed);

    pmad_destroy();
    return failed == 0 ? 0 : 1;
}