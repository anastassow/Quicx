#ifndef DISPACHER_H
#define DISPACHER_H

#include "quicx.h"

/* function to dispatch a task to a idle worker */
void try_dispach(Quicx* quicx);

#endif