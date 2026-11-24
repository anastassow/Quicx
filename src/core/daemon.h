#ifndef DAEMON_H
#define DAEMON_H

#include "net/server.h"
#include "qxp/protocol.h"
#include "core/quicx.h"

/* Function that handles every message type */
void daemon_handle_message(Connection* conn, Message* msg, Quicx* quicx);

/* Function to disconnect and clean up a worker connection */
void daemon_handle_disconnect(Connection* conn, Quicx* quicx);

#endif