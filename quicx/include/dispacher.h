#ifndef DISPACHER_H
#define DISPACHER_H

#include "queue.h"
#include "workers.h"

/* function to dispatch a task to a idle worker */
void try_dispach(Queue* q, WorkerPool* pool);

#endif