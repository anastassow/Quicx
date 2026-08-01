#include "platform.h"

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
#include "stats.h"
#include "quicx.h"
#include "event_loop.h"

#define TEST_PORT 16382

// ── server ────────────────────────────────────────────────────────────────────

int server_start(int port, Quicx* quicx);

static Quicx g_quicx;

static void* server_thread(void* arg) {
    (void)arg;
    server_start(TEST_PORT, &g_quicx);
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

// read full stats response — returns 1 ok, 0 disconnect, -1 error
static int read_stats(int fd, StatsHeader* hdr,
                      StatsClass* classes, int max_classes,
                      int* class_count) {
    // read protocol header
    ProtocolHeader phdr;
    int r = protocol_read_exact(fd, &phdr, PROTOCOL_HEADER_SIZE);
    if (r <= 0) return r;

    if ((MessageType)phdr.type != MSG_STATS_RESPONSE) return -1;

    uint32_t total_len = ntohl(phdr.length);

    // read StatsHeader
    r = protocol_read_exact(fd, hdr, sizeof(StatsHeader));
    if (r <= 0) return r;

    // read StatsClass array
    uint32_t n = hdr->class_count;
    if ((int)n > max_classes) return -1;

    if (n > 0) {
        r = protocol_read_exact(fd, classes,
                                (int)(n * sizeof(StatsClass)));
        if (r <= 0) return r;
    }

    *class_count = (int)n;
    (void)total_len;
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

static void test_stats_initial(void) {
    printf("\n--- test: initial stats — no workers no tasks ---\n");

    int fd = connect_to_server();
    set_timeout(fd, 2);

    protocol_send_stats_request(fd);

    StatsHeader hdr;
    StatsClass  classes[8];
    int         n = 0;
    int r = read_stats(fd, &hdr, classes, 8, &n);

    check("got stats response",      r == 1);
    check("idle workers is 0",       ntohl(hdr.idle_workers) == 0);
    check("busy workers is 0",       ntohl(hdr.busy_workers) == 0);
    check("queued tasks is 0",       ntohl(hdr.queued_tasks) == 0);
    check("submitted is 0",          ntohll(hdr.tasks_submitted) == 0);
    check("completed is 0",          ntohll(hdr.tasks_completed) == 0);
    check("failed is 0",             ntohll(hdr.tasks_failed) == 0);
    check("has size classes",        n > 0);
    check("uptime >= 0",             ntohll(hdr.uptime_seconds) >= 0);

    printf("  class count: %d\n", n);
    for (int i = 0; i < n; i++) {
        printf("  class %d: size=%u total=%u used=%u\n",
               i,
               ntohl(classes[i].block_size),
               ntohl(classes[i].total_blocks),
               ntohl(classes[i].allocated_blocks));
    }

    close(fd);
}

static void test_stats_with_worker(void) {
    printf("\n--- test: stats reflect worker connected ---\n");

    int worker_fd = connect_to_server();
    int stats_fd  = connect_to_server();
    set_timeout(worker_fd, 2);
    set_timeout(stats_fd,  2);

    // register worker
    protocol_send_ready(worker_fd);
    usleep(20000);

    // request stats
    protocol_send_stats_request(stats_fd);

    StatsHeader hdr;
    StatsClass  classes[8];
    int         n = 0;
    int r = read_stats(stats_fd, &hdr, classes, 8, &n);

    check("got stats response",  r == 1);
    check("idle workers is 1",   ntohl(hdr.idle_workers) == 1);
    check("busy workers is 0",   ntohl(hdr.busy_workers) == 0);
    check("queued tasks is 0",   ntohl(hdr.queued_tasks) == 0);

    close(worker_fd);
    close(stats_fd);
}

static void test_stats_submitted_counter(void) {
    printf("\n--- test: submitted counter increments ---\n");

    int worker_fd   = connect_to_server();
    int producer_fd = connect_to_server();
    int stats_fd    = connect_to_server();
    set_timeout(worker_fd,   2);
    set_timeout(producer_fd, 2);
    set_timeout(stats_fd,    2);

    // register worker
    protocol_send_ready(worker_fd);
    usleep(20000);

    // submit 3 tasks
    for (int i = 0; i < 3; i++) {
        protocol_send_submit(producer_fd, "test_task",
                             (const uint8_t*)"data", 4);
        // drain OK response
        ProtocolHeader ok_phdr;
        protocol_read_exact(producer_fd, &ok_phdr,
                            PROTOCOL_HEADER_SIZE);
        uint32_t ok_id;
        protocol_read_exact(producer_fd, &ok_id, 4);

        // drain TASK from worker and send DONE
        ProtocolHeader task_phdr;
        protocol_read_exact(worker_fd, &task_phdr,
                            PROTOCOL_HEADER_SIZE);
        uint32_t task_len = ntohl(task_phdr.length);
        uint8_t  task_buf[256];
        if (task_len > 0 && task_len < sizeof(task_buf))
            protocol_read_exact(worker_fd, task_buf,
                                (int)task_len);

        // parse task_id from buf
        uint32_t task_id;
        memcpy(&task_id, task_buf, 4);
        protocol_send_done(worker_fd, task_id);
        usleep(10000);
    }

    usleep(20000);

    // request stats
    protocol_send_stats_request(stats_fd);

    StatsHeader hdr;
    StatsClass  classes[8];
    int         n = 0;
    int r = read_stats(stats_fd, &hdr, classes, 8, &n);

    check("got stats response",       r == 1);
    check("submitted is 3",           ntohll(hdr.tasks_submitted) == 3);
    check("completed is 3",           ntohll(hdr.tasks_completed) == 3);
    check("failed is 0",              ntohll(hdr.tasks_failed) == 0);

    close(worker_fd);
    close(producer_fd);
    close(stats_fd);
}

static void test_stats_busy_worker(void) {
    printf("\n--- test: stats show busy worker during task ---\n");

    int worker_fd   = connect_to_server();
    int producer_fd = connect_to_server();
    int stats_fd    = connect_to_server();
    set_timeout(worker_fd,   2);
    set_timeout(producer_fd, 2);
    set_timeout(stats_fd,    2);

    // get baseline stats before test
    protocol_send_stats_request(stats_fd);
    StatsHeader baseline;
    StatsClass  classes[8];
    int         n = 0;
    read_stats(stats_fd, &baseline, classes, 8, &n);
    uint64_t base_submitted = ntohll(baseline.tasks_submitted);
    uint64_t base_completed = ntohll(baseline.tasks_completed);

    // register worker
    protocol_send_ready(worker_fd);
    usleep(20000);

    // submit task — worker becomes busy
    protocol_send_submit(producer_fd, "slow_task",
                         (const uint8_t*)"x", 1);

    // drain OK
    ProtocolHeader ok_phdr;
    protocol_read_exact(producer_fd, &ok_phdr, PROTOCOL_HEADER_SIZE);
    uint32_t ok_id;
    protocol_read_exact(producer_fd, &ok_id, 4);

    // drain TASK from worker — do NOT send DONE yet
    ProtocolHeader task_phdr;
    protocol_read_exact(worker_fd, &task_phdr, PROTOCOL_HEADER_SIZE);
    uint32_t task_len = ntohl(task_phdr.length);
    uint8_t  task_buf[256];
    if (task_len > 0 && task_len < sizeof(task_buf))
        protocol_read_exact(worker_fd, task_buf, (int)task_len);

    usleep(20000);

    // request stats — worker should be busy
    protocol_send_stats_request(stats_fd);
    StatsHeader hdr;
    int r = read_stats(stats_fd, &hdr, classes, 8, &n);

    check("got stats response",  r == 1);
    check("idle workers is 0",   ntohl(hdr.idle_workers) == 0);
    check("busy workers is 1",   ntohl(hdr.busy_workers) == 1);
    check("submitted +1",        ntohll(hdr.tasks_submitted) ==
                                 base_submitted + 1);
    check("completed unchanged", ntohll(hdr.tasks_completed) ==
                                 base_completed);

    // finish the task
    uint32_t task_id;
    memcpy(&task_id, task_buf, 4);
    protocol_send_done(worker_fd, task_id);
    usleep(20000);

    // stats should update
    protocol_send_stats_request(stats_fd);
    r = read_stats(stats_fd, &hdr, classes, 8, &n);

    check("after done — idle is 1",      ntohl(hdr.idle_workers) == 1);
    check("after done — busy is 0",      ntohl(hdr.busy_workers) == 0);
    check("after done — completed +1",   ntohll(hdr.tasks_completed) ==
                                         base_completed + 1);

    close(worker_fd);
    close(producer_fd);
    close(stats_fd);
}

static void test_stats_pmad_classes(void) {
    printf("\n--- test: PMAD size classes in stats ---\n");

    int fd = connect_to_server();
    set_timeout(fd, 2);

    protocol_send_stats_request(fd);

    StatsHeader hdr;
    StatsClass  classes[8];
    int         n = 0;
    int r = read_stats(fd, &hdr, classes, 8, &n);  // ← fd not stats_fd

    check("got stats response",     r == 1);
    check("has size classes",       n > 0);

    uint64_t total_bytes = 0;
    uint64_t used_bytes  = 0;

    for (int i = 0; i < n; i++) {
        uint32_t bs    = ntohl(classes[i].block_size);
        uint32_t total = ntohl(classes[i].total_blocks);
        uint32_t alloc = ntohl(classes[i].allocated_blocks);
        uint32_t free  = total - alloc;

        check("block_size > 0",    bs > 0);
        check("total_blocks > 0",  total > 0);
        check("used <= total",     alloc <= total);

        total_bytes += (uint64_t)bs * total;
        used_bytes  += (uint64_t)bs * alloc;

        printf("  %4uB   total: %4u   used: %4u   free: %4u\n",
               bs, total, alloc, free);
    }

    printf("  memory total: %llu bytes\n",
           (unsigned long long)total_bytes);
    printf("  memory used:  %llu bytes\n",
           (unsigned long long)used_bytes);

    check("total memory > 0",  total_bytes > 0);
    check("used <= total",     used_bytes <= total_bytes);

    close(fd);
}

static void test_stats_uptime(void) {
    printf("\n--- test: uptime increases ---\n");

    int fd1 = connect_to_server();
    set_timeout(fd1, 2);

    protocol_send_stats_request(fd1);

    StatsHeader hdr1;
    StatsClass  classes[8];
    int         n = 0;
    read_stats(fd1, &hdr1, classes, 8, &n);
    close(fd1);

    uint64_t uptime1 = ntohll(hdr1.uptime_seconds);

    sleep(2);

    int fd2 = connect_to_server();
    set_timeout(fd2, 2);

    protocol_send_stats_request(fd2);

    StatsHeader hdr2;
    read_stats(fd2, &hdr2, classes, 8, &n);
    close(fd2);

    uint64_t uptime2 = ntohll(hdr2.uptime_seconds);

    check("uptime increased",    uptime2 > uptime1);
    check("uptime diff >= 2s",   uptime2 - uptime1 >= 2);

    printf("  uptime1: %llus\n", (unsigned long long)uptime1);
    printf("  uptime2: %llus\n", (unsigned long long)uptime2);
}

static void test_stats_pmad_usage(void) {
    printf("\n--- test: PMAD usage with real load ---\n");

    // connect 5 workers
    int workers[5];
    for (int i = 0; i < 5; i++) {
        workers[i] = connect_to_server();
        set_timeout(workers[i], 2);
        protocol_send_ready(workers[i]);
    }
    usleep(30000);

    // connect 5 producers and submit 5 tasks each = 25 tasks
    int producers[5];
    uint32_t task_ids[25];
    int task_count = 0;

    for (int i = 0; i < 5; i++) {
        producers[i] = connect_to_server();
        set_timeout(producers[i], 2);
    }

    for (int i = 0; i < 5; i++) {
        for (int j = 0; j < 5; j++) {
            const uint8_t* payload = (const uint8_t*)
                "{\"key\":\"value\",\"index\":99}";
            protocol_send_submit(producers[i], "load_test",
                                 payload,
                                 (uint32_t)strlen((char*)payload));

            // read OK
            ProtocolHeader ok_phdr;
            protocol_read_exact(producers[i], &ok_phdr,
                                PROTOCOL_HEADER_SIZE);
            uint32_t net_id;
            protocol_read_exact(producers[i], &net_id, 4);
            task_ids[task_count++] = ntohl(net_id);
        }
    }

    usleep(30000);

    // check stats with load
    int stats_fd = connect_to_server();
    set_timeout(stats_fd, 2);
    protocol_send_stats_request(stats_fd);

    StatsHeader hdr;
    StatsClass  classes[8];
    int         n = 0;
    int r = read_stats(stats_fd, &hdr, classes, 8, &n);

    check("got stats response", r == 1);

    uint64_t total_bytes = 0;
    uint64_t used_bytes  = 0;

    printf("\n  PMAD under load:\n");
    for (int i = 0; i < n; i++) {
        uint32_t bs    = ntohl(classes[i].block_size);
        uint32_t total = ntohl(classes[i].total_blocks);
        uint32_t alloc = ntohl(classes[i].allocated_blocks);
        uint32_t free  = total - alloc;

        total_bytes += (uint64_t)bs * total;
        used_bytes  += (uint64_t)bs * alloc;

        // draw a usage bar
        int bar_total = 20;
        int bar_used  = total > 0
                        ? (int)((uint64_t)alloc * bar_total / total)
                        : 0;
        printf("  %4uB  [", bs);
        for (int b = 0; b < bar_total; b++)
            printf("%s", b < bar_used ? "█" : "░");
        printf("]  %u / %u\n", alloc, total);
    }

    printf("\n  total: %llu bytes\n",
           (unsigned long long)total_bytes);
    printf("  used:  %llu bytes  (%.1f%%)\n",
           (unsigned long long)used_bytes,
           total_bytes > 0
               ? (double)used_bytes * 100.0 / (double)total_bytes
               : 0.0);

    check("used_bytes > 0",         used_bytes > 0);
    check("used_bytes <= total",     used_bytes <= total_bytes);

    // drain all worker tasks and send DONE
    for (int i = 0; i < 5; i++) {
        // each worker may have received tasks
        // read until timeout
        while (1) {
            ProtocolHeader task_phdr;
            int rr = protocol_read_exact(workers[i],
                                         &task_phdr,
                                         PROTOCOL_HEADER_SIZE);
            if (rr <= 0) break;

            uint32_t tlen = ntohl(task_phdr.length);
            uint8_t  tbuf[512];
            if (tlen > 0 && tlen < sizeof(tbuf)) {
                protocol_read_exact(workers[i], tbuf, (int)tlen);
                uint32_t tid;
                memcpy(&tid, tbuf, 4);
                protocol_send_done(workers[i], tid);
            }
        }
    }

    usleep(30000);

    // check stats after all tasks done — memory should drop
    protocol_send_stats_request(stats_fd);
    StatsHeader after;
    read_stats(stats_fd, &after, classes, 8, &n);

    uint64_t used_after = 0;
    for (int i = 0; i < n; i++) {
        uint32_t bs    = ntohl(classes[i].block_size);
        uint32_t alloc = ntohl(classes[i].allocated_blocks);
        used_after += (uint64_t)bs * alloc;
    }

    printf("\n  after all DONE:\n");
    for (int i = 0; i < n; i++) {
        uint32_t bs    = ntohl(classes[i].block_size);
        uint32_t total = ntohl(classes[i].total_blocks);
        uint32_t alloc = ntohl(classes[i].allocated_blocks);
        uint32_t free  = total - alloc;

        int bar_total = 20;
        int bar_used  = total > 0
                        ? (int)((uint64_t)alloc * bar_total / total)
                        : 0;
        printf("  %4uB  [", bs);
        for (int b = 0; b < bar_total; b++)
            printf("%s", b < bar_used ? "█" : "░");
        printf("]  %u / %u\n", alloc, total);
    }

    printf("\n  used after: %llu bytes  (%.1f%%)\n",
           (unsigned long long)used_after,
           total_bytes > 0
               ? (double)used_after * 100.0 / (double)total_bytes
               : 0.0);

    check("memory drops after tasks done", used_after < used_bytes);

    // cleanup
    close(stats_fd);
    for (int i = 0; i < 5; i++) {
        close(workers[i]);
        close(producers[i]);
    }
}

// ── main ──────────────────────────────────────────────────────────────────────

int main(void) {
    printf("=== stats tests ===\n");
    printf("backend: %s\n", EL_BACKEND);

    size_t sizes[]       = {32, 64, 128, 256, 512, 1024};
    size_t percentages[] = {10, 25,  25,  20,  12,    8};
    pmad_init(&g_quicx.pmad, sizes, 6, percentages, 1024 * 1024);

    stats_init(&g_quicx.stats);
    queue_init(&g_quicx.queue);
    workers_init(&g_quicx.pool);

    pthread_t srv;
    pthread_create(&srv, NULL, server_thread, NULL);
    pthread_detach(srv);

    usleep(100000);  // wait for server

    test_stats_initial();
    test_stats_with_worker();
    test_stats_submitted_counter();
    test_stats_busy_worker();
    test_stats_pmad_classes();
    test_stats_uptime();
    test_stats_pmad_usage();

    printf("\n=== results ===\n");
    printf("passed: %d\n", passed);
    printf("failed: %d\n", failed);

    if (failed == 0)
        printf("--- all stats tests passed ---\n");
    else
        printf("--- %d tests FAILED ---\n", failed);

    pmad_destroy(&g_quicx.pmad);
    return failed == 0 ? 0 : 1;
}