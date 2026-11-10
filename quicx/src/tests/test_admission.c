/*
admission tests: value levels in the queue, and which waiting task (if any)
admission_make_room evicts when the task budget is full. Runs the real
queue, apool and admission code on a small budget, without the network.
*/
#include "platform.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <arpa/inet.h>

#include "quicx.h"
#include "admission.h"
#include "protocol.h"

static int passed = 0, failed = 0;
static void check(const char* name, int ok) {
    if (ok) { printf("  PASS: %s\n", name); passed++; }
    else    { printf("  FAIL: %s\n", name); failed++; }
}

#define SMALL 48     /* payload bytes: a 112-byte block, 146 per page */
#define BIG   1000   /* payload bytes: a 1072-byte block, 15 per page */

static Quicx Q;

static void start(size_t pool, AdmissionMode mode) {
    Config c;
    config_defaults(&c);
    c.pool.pool_size = pool;
    c.pool.admission = (uint8_t)mode;
    if (quicx_init(&Q, &c) != APOOL_OK) { printf("quicx_init failed\n"); _exit(1); }
}

static void stop(void) { quicx_destroy(&Q); }

/* allocates and queues a task as the daemon does; NULL when the budget is full */
static Task* add(size_t payload, uint8_t level, int producer_fd) {
    Task* t = (Task*)apool_alloc(&Q.mem, sizeof(Task) + payload);
    if (!t) return NULL;
    memset(t, 0, sizeof(Task));
    t->id = queue_next_id(&Q.queue);
    t->level = level;
    t->payload = payload ? (void*)(t + 1) : NULL;
    t->payload_len = (uint32_t)payload;
    t->producer_fd = producer_fd;
    queue_push(&Q.queue, t);
    return t;
}

/* fills the budget with tasks of one size, cycling through the given levels */
static int fill(size_t payload, const uint8_t* levels, int nlevels) {
    int n = 0;
    while (add(payload, levels[n % nlevels], -1)) n++;
    return n;
}

typedef struct { uint32_t id; int found; } Find;
static void find_id(void* ctx, Task* t) { Find* f = ctx; if (t->id == f->id) f->found = 1; }
static int queued(uint32_t id) { Find f = { id, 0 }; queue_for_each(&Q.queue, find_id, &f); return f.found; }

static size_t block(size_t payload) { return sizeof(Task) + payload; }

static void test_order(void) {
    printf("\n--- dispatch order ---\n");
    start(256 * 1024, ADMISSION_VALUE);
    for (int i = 0; i < 5; i++) add(SMALL, QUEUE_LEVEL_DEFAULT, -1);
    int fifo = 1;
    for (uint32_t id = 1; id <= 5; id++) { Task* t = queue_pop(&Q.queue); fifo &= t && t->id == id; apool_free(&Q.mem, t); }
    check("one level is a plain FIFO", fifo);

    Task* a = add(SMALL, 3, -1); Task* b = add(SMALL, 9, -1);
    Task* c = add(SMALL, 3, -1); Task* d = add(SMALL, 9, -1);
    uint32_t want[4] = { b->id, d->id, a->id, c->id };
    int prio = 1;
    for (int i = 0; i < 4; i++) { Task* t = queue_pop(&Q.queue); prio &= t && t->id == want[i]; apool_free(&Q.mem, t); }
    check("higher level first, oldest first within a level", prio);
    check("queue empty after", queue_pop(&Q.queue) == NULL && queue_size(&Q.queue) == 0);
    stop();
}

static void test_refuse(void) {
    printf("\n--- admission = refuse ---\n");
    start(256 * 1024, ADMISSION_REFUSE);
    uint8_t lv[1] = { 2 };
    int n = fill(SMALL, lv, 1);
    check("budget holds 1,752 tasks of 48 B (as calculated)", n == 1752);
    check("a level 15 task is still refused", admission_make_room(&Q, block(SMALL), 15) == 0);
    check("nothing evicted", Q.stats.tasks_evicted == 0 && queue_size(&Q.queue) == n);
    stop();
}

static void test_value(void) {
    printf("\n--- admission = value ---\n");
    start(256 * 1024, ADMISSION_VALUE);
    uint8_t lv[2] = { 5, 3 };                 /* ids 1,3,5.. level 5; ids 2,4,6.. level 3 */
    int n = fill(SMALL, lv, 2);
    check("equal level does not displace", admission_make_room(&Q, block(SMALL), 3) == 0);
    check("level 4 evicts the oldest level 3 task", admission_make_room(&Q, block(SMALL), 4) == 1 && !queued(2) && queued(1));
    check("the freed block takes the new task", add(SMALL, 4, -1) != NULL);
    check("level 9 evicts the next oldest level 3 task", admission_make_room(&Q, block(SMALL), 9) == 1 && !queued(4));
    add(SMALL, 9, -1);
    check("evictions are counted", Q.stats.tasks_evicted == 2);
    check("queue size is unchanged by evict + add", queue_size(&Q.queue) == n);
    stop();

    start(256 * 1024, ADMISSION_VALUE);
    uint8_t crit[1] = { QUEUE_LEVEL_CRITICAL };
    fill(SMALL, crit, 1);
    check("critical tasks are never evicted, even for critical", admission_make_room(&Q, block(SMALL), QUEUE_LEVEL_CRITICAL) == 0);
    stop();
}

static void test_oldest(void) {
    printf("\n--- admission = oldest ---\n");
    start(256 * 1024, ADMISSION_OLDEST);
    uint8_t lv[2] = { 9, 2 };
    fill(SMALL, lv, 2);
    check("evicts the oldest task, whatever its level", admission_make_room(&Q, block(SMALL), 0) == 1 && !queued(1) && queued(2));
    stop();

    start(256 * 1024, ADMISSION_OLDEST);
    uint8_t crit[1] = { QUEUE_LEVEL_CRITICAL };
    fill(SMALL, crit, 1);
    check("never evicts a critical task", admission_make_room(&Q, block(SMALL), 0) == 0);
    stop();
}

static void test_room(void) {
    printf("\n--- only memory the new task can use ---\n");
    /* 96 KiB: two task pages. One big task alone in one page, small tasks fill the other */
    start(96 * 1024, ADMISSION_VALUE);
    Task* big = add(BIG, 2, -1);
    uint32_t big_id = big->id;
    uint8_t lv[1] = { 5 };
    fill(SMALL, lv, 1);
    check("budget is full for a small task", apool_alloc(&Q.mem, block(SMALL)) == NULL);
    check("the lone big task's page would come free", apool_free_makes_room(&Q.mem, big, block(SMALL)) == 1);
    check("it is evicted for a level 3 small task", admission_make_room(&Q, block(SMALL), 3) == 1 && !queued(big_id));
    check("and the small task fits", add(SMALL, 3, -1) != NULL);
    stop();

    start(96 * 1024, ADMISSION_VALUE);
    Task* b1 = add(BIG, 2, -1);
    add(BIG, 2, -1);                          /* two live blocks: freeing one empties nothing */
    fill(SMALL, lv, 1);
    int before = queue_size(&Q.queue);
    check("a big task's block cannot hold a small task", apool_free_makes_room(&Q.mem, b1, block(SMALL)) == 0);
    check("so nothing is evicted for a level 3 small task", admission_make_room(&Q, block(SMALL), 3) == 0);
    check("queue untouched", queue_size(&Q.queue) == before && Q.stats.tasks_evicted == 0);
    stop();
}

static void test_notice(void) {
    printf("\n--- the evicted task's producer is told ---\n");
    int sv[2];
    socketpair(AF_UNIX, SOCK_STREAM, 0, sv);
    start(256 * 1024, ADMISSION_VALUE);
    Task* first = add(SMALL, 1, sv[0]);
    uint32_t id = first->id;
    uint8_t lv[1] = { 1 };
    fill(SMALL, lv, 1);
    check("eviction made room", admission_make_room(&Q, block(SMALL), 8) == 1);
    uint8_t buf[64];
    ssize_t n = read(sv[1], buf, sizeof buf);
    uint32_t got;
    memcpy(&got, buf + 6, 4);
    check("producer receives MSG_FAILED", n >= 10 && buf[1] == MSG_FAILED);
    check("for the evicted task's id", ntohl(got) == id);
    check("with reason 'evicted'", n >= 17 && memcmp(buf + 10, "evicted", 7) == 0);
    stop();
    close(sv[0]); close(sv[1]);
}

int main(void) {
    printf("=== admission tests ===\n");
    test_order();
    test_refuse();
    test_value();
    test_oldest();
    test_room();
    test_notice();
    printf("\n=== results ===\npassed: %d\nfailed: %d\n", passed, failed);
    return failed == 0 ? 0 : 1;
}
