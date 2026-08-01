#ifndef DAEMON_H
#define DAEMON_H

#include "server.h"
#include "protocol.h"
#include "quicx.h"

/* Function that handles every message type */
void daemon_handle_message(Connection* conn, Message* msg, Quicx* quicx);

/* Function to disconnect and clean up a worker connection */
void daemon_handle_disconnect(Connection* conn, Quicx* quicx);

#endif