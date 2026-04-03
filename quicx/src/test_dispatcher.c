#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include "queue.h"
#include "workers.h"
#include "dispacher.h"
#include "incPMAD.h"

// helper — create a fake worker using a pipe
// pipe[0] = read end, pipe[1] = write end
// dispatcher writes to pipe[1] via w->fd
// test reads from pipe[0] to verify what was sent
static Worker* make_fake_worker(WorkerPool* pool, int pipe_fds[2]) {
    pipe(pipe_fds);
    return worker_add(pool, pipe_fds[1]);  // worker fd = write end
}

// helper — read what dispatcher sent to the worker
static void read_message(int read_fd, char* buf, int maxlen) {
    int n = read(read_fd, buf, maxlen - 1);
    if (n < 0) n = 0;
    buf[n] = '\0';
}

int main() {
    // init PMAD
    size_t sizes[]       = {32, 64, 128, 256, 1024};
    size_t percentages[] = {20, 30, 20,  20,  10};
    pmad_init(sizes, percentages);

    Queue      q;
    WorkerPool pool;
    queue_init(&q);
    workers_init(&pool);

    printf("=== test 1: no workers, task arrives — nothing dispatched ===\n");
    {
        Task* t = (Task*)pmad_alloc(sizeof(Task));
        t->id          = queue_next_id(&q);
        t->payload_len = 0;
        t->payload     = NULL;
        strncpy(t->type, "send_email", sizeof(t->type));
        queue_push(&q, t);

        try_dispach(&q, &pool);  // no workers — task stays in queue

        printf("queue size: %d (expect 1)\n", queue_size(&q));
        printf("idle:       %d (expect 0)\n", workers_idle_count(&pool));

        // cleanup
        Task* leftover = queue_pop(&q);
        pmad_free(leftover);
    }

    printf("\n=== test 2: one worker, one task — dispatched correctly ===\n");
    {
        int pipe_fds[2];
        Worker* w = make_fake_worker(&pool, pipe_fds);

        Task* t = (Task*)pmad_alloc(sizeof(Task));
        t->id          = queue_next_id(&q);
        t->payload_len = 5;
        t->payload     = "hello";
        strncpy(t->type, "send_email", sizeof(t->type));
        queue_push(&q, t);

        printf("before dispatch — idle: %d (expect 1)\n",
               workers_idle_count(&pool));

        try_dispach(&q, &pool);

        printf("after dispatch  — idle: %d (expect 0)\n",
               workers_idle_count(&pool));
        printf("after dispatch  — busy: %d (expect 1)\n",
               workers_busy_count(&pool));
        printf("queue size:       %d (expect 0)\n", queue_size(&q));

        // read what was sent to the worker
        char msg[256];
        read_message(pipe_fds[0], msg, sizeof(msg));
        printf("message sent: %s", msg);
        printf("(expect: TASK %u send_email hello)\n", t->id);

        // cleanup
        pmad_free(t);
        worker_remove(&pool, w);
        close(pipe_fds[0]);
    }

    printf("\n=== test 3: two workers, two tasks — both dispatched ===\n");
    {
        int pipe_a[2], pipe_b[2];
        Worker* wa = make_fake_worker(&pool, pipe_a);
        Worker* wb = make_fake_worker(&pool, pipe_b);

        Task* t1 = (Task*)pmad_alloc(sizeof(Task));
        t1->id          = queue_next_id(&q);
        t1->payload_len = 3;
        t1->payload     = "foo";
        strncpy(t1->type, "task_a", sizeof(t1->type));

        Task* t2 = (Task*)pmad_alloc(sizeof(Task));
        t2->id          = queue_next_id(&q);
        t2->payload_len = 3;
        t2->payload     = "bar";
        strncpy(t2->type, "task_b", sizeof(t2->type));

        queue_push(&q, t1);
        queue_push(&q, t2);

        printf("before dispatch — idle: %d (expect 2)\n",
               workers_idle_count(&pool));

        try_dispach(&q, &pool);

        printf("after dispatch  — idle: %d (expect 0)\n",
               workers_idle_count(&pool));
        printf("after dispatch  — busy: %d (expect 2)\n",
               workers_busy_count(&pool));
        printf("queue size:       %d (expect 0)\n", queue_size(&q));

        char msg_a[256], msg_b[256];
        read_message(pipe_a[0], msg_a, sizeof(msg_a));
        read_message(pipe_b[0], msg_b, sizeof(msg_b));
        printf("worker a got: %s", msg_a);
        printf("worker b got: %s", msg_b);

        // cleanup
        pmad_free(t1);
        pmad_free(t2);
        worker_remove(&pool, wa);
        worker_remove(&pool, wb);
        close(pipe_a[0]);
        close(pipe_b[0]);
    }

    printf("\n=== test 4: worker idle, no tasks — nothing dispatched ===\n");
    {
        int pipe_fds[2];
        Worker* w = make_fake_worker(&pool, pipe_fds);

        printf("queue size: %d (expect 0)\n", queue_size(&q));
        try_dispach(&q, &pool);
        printf("idle: %d (expect 1)\n", workers_idle_count(&pool));
        printf("busy: %d (expect 0)\n", workers_busy_count(&pool));

        worker_remove(&pool, w);
        close(pipe_fds[0]);
        close(pipe_fds[1]);
    }

    pmad_destroy();
    printf("\n--- all dispatcher tests passed ---\n");
    return 0;
}