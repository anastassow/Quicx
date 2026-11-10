#ifndef ADMISSION_H
#define ADMISSION_H

#include <stddef.h>
#include <stdint.h>

/*
What Quicx does when the task budget is full and a new task arrives.

  ADMISSION_REFUSE  the new task gets ERROR 0x01 (1.x and 2.0 behaviour)
  ADMISSION_OLDEST  the oldest waiting task makes room, as a leaky queue that
                    drops old frames would; critical tasks are never dropped
  ADMISSION_VALUE   a waiting task of a lower value level makes room, the
                    lowest level and then the oldest first; equal or higher
                    levels are never displaced, so with one level this is
                    exactly ADMISSION_REFUSE

In every mode a task is only evicted when apool can prove its memory lets the
new task in (apool_free_makes_room): same block size, or the last block of a
page the new size may take. A task is never thrown away for nothing.
*/
typedef enum {
    ADMISSION_REFUSE = 0,
    ADMISSION_OLDEST = 1,
    ADMISSION_VALUE  = 2,
} AdmissionMode;

#define ADMISSION_DEFAULT ADMISSION_VALUE

/* how deep each level is searched for a usable victim */
#define ADMISSION_SCAN 64

struct Quicx;

/*
called when apool_alloc(size) for a new task of value `level` failed.
evicts at most one waiting task (telling its producer with MSG_FAILED
"evicted") and returns 1 when the allocation will now succeed, else 0.
*/
int admission_make_room(struct Quicx* quicx, size_t size, uint8_t level);

#endif
