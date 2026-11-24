#ifndef DISPATCHER_H
#define DISPATCHER_H

#include "core/quicx.h"

/* function to dispatch a task to a idle worker */
void try_dispatch(Quicx* quicx);

#endif