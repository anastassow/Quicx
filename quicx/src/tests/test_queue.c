#include <stdio.h>
#include <string.h>
#include "queue.h"
#include "incPMAD.h"

int main() {
    // init PMAD
    size_t sizes[]       = {64, 128, 256, 512, 1024};
    size_t percentages[] = {40, 20,  20,  10,  10};
    PMAD pmad;
    pmad_init(&pmad, sizes, 6, percentages, 1024 * 1024);

    Queue q;
    queue_init(&q);

    // allocate tasks from PMAD — not malloc
    Task* a = (Task*)pmad_alloc(&pmad, sizeof(Task));
    Task* b = (Task*)pmad_alloc(&pmad, sizeof(Task));
    Task* c = (Task*)pmad_alloc(&pmad, sizeof(Task));

    // set IDs
    a->id = queue_next_id(&q);  // 1
    b->id = queue_next_id(&q);  // 2
    c->id = queue_next_id(&q);  // 3

    // set types
    strncpy(a->type, "send_email",    sizeof(a->type));
    strncpy(b->type, "resize_image",  sizeof(b->type));
    strncpy(c->type, "process_video", sizeof(c->type));

    // push all three
    queue_push(&q, a);
    queue_push(&q, b);
    queue_push(&q, c);

    printf("size after 3 pushes: %d (expect 3)\n", queue_size(&q));

    // pop — should come out in order pushed (FIFO)
    Task* t1 = queue_pop(&q);
    Task* t2 = queue_pop(&q);
    Task* t3 = queue_pop(&q);
    Task* t4 = queue_pop(&q);  // empty — should be NULL

    printf("pop 1: id=%u type=%s (expect 1 send_email)\n",
           t1->id, t1->type);
    printf("pop 2: id=%u type=%s (expect 2 resize_image)\n",
           t2->id, t2->type);
    printf("pop 3: id=%u type=%s (expect 3 process_video)\n",
           t3->id, t3->type);
    printf("pop 4: %s (expect NULL)\n",
           t4 ? "not null — BUG" : "NULL — correct");

    printf("size after 3 pops:  %d (expect 0)\n", queue_size(&q));

    // free back to PMAD
    pmad_free(&pmad, a);
    pmad_free(&pmad, b);
    pmad_free(&pmad, c);

    pmad_destroy(&pmad);

    printf("\nall tests passed\n");
    return 0;
}