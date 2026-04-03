#ifndef DISPACHER_H
#define DISPACHER_H

#include "queue.h"
#include "workers.h"

void try_dispach(Queue* q, WorkerPool* pool);

#endif