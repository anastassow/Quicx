#include "server.h"
#include "event_loop.h"
#include "protocol.h"
#include "daemon.h"
#include "incPMAD.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <arpa/inet.h>
#include <netinet/tcp.h>
#include <sys/socket.h>

// connection table-----------------------------------
// O(1) lookup

static Connection connections[MAX_CONNECTIONS];

// socket helpers-------------------------------------

static int set_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) return -1;
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

static void set_nodelay(int fd) {
    int flag = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY,
               &flag, sizeof(flag));
}

static void set_reuseaddr(int fd) {
    int flag = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR,
               &flag, sizeof(flag));
}

// connection managment------------------------------

static void conn_init(int fd) {
    connections[fd].fd     = fd;
    connections[fd].type   = CONN_UNKNOWN;
    connections[fd].worker = NULL;
}

static void conn_close(int loop_fd, int fd, WorkerPool* pool) {
    Connection* conn = &connections[fd];

    // notify daemon so it can clean up worker state
    // this is from the deamon for now it is not made i will make it afterwords !!!
    daemon_handle_disconnect(conn, pool);

    el_remove(loop_fd, fd);

    close(fd);

    conn->fd     = -1;
    conn->type   = CONN_UNKNOWN;
    conn->worker = NULL;

    printf("connection closed fd=%d\n", fd);
}

static void handle_new_connection(int loop_fd, int server_fd) {
    struct sockaddr_in addr;
    socklen_t addrlen = sizeof(addr);

    int fd = accept(server_fd,
                    (struct sockaddr*)&addr,
                    &addrlen);
    if (fd < 0) {
        perror("accept");
        return;
    }

    if (fd >= MAX_CONNECTIONS) {
        fprintf(stderr, "too many connections fd=%d\n", fd);
        close(fd);
        return;
    }

    set_nonblocking(fd);
    set_nodelay(fd);

    conn_init(fd);

    el_add(loop_fd, fd, &connections[fd]);

    printf("new connection fd=%d from %s:%d\n",
           fd,
           inet_ntoa(addr.sin_addr),
           ntohs(addr.sin_port));
}

static void handle_readable(int loop_fd, Connection* conn,
                             Queue* q, WorkerPool* pool) {

    ProtocolHeader hdr;
    int r = protocol_read_exact(conn->fd,
                                &hdr,
                                PROTOCOL_HEADER_SIZE);
    if (r == 0) {
        conn_close(loop_fd, conn->fd, pool);
        return;
    }
    if (r < 0) {
        conn_close(loop_fd, conn->fd, pool);
        return;
    }

    if (hdr.version != PROTOCOL_VERSION) {
        protocol_send_error(conn->fd,
                            ERR_VERSION_MISMATCH,
                            "bad version");
        conn_close(loop_fd, conn->fd, pool);
        return;
    }

    uint32_t payload_len = ntohl(hdr.length);

    if (payload_len > PROTOCOL_MAX_PAYLOAD) {
        protocol_send_error(conn->fd,
                            ERR_PAYLOAD_TOO_BIG,
                            "payload too large");
        conn_close(loop_fd, conn->fd, pool);
        return;
    }

    uint8_t* buf = NULL;
    if (payload_len > 0) {
        buf = (uint8_t*)pmad_alloc(payload_len);
        if (!buf) {
            protocol_send_error(conn->fd,
                                ERR_QUEUE_FULL,
                                "out of memory");
            return;
        }

        r = protocol_read_exact(conn->fd,
                                buf,
                                (int)payload_len);
        if (r <= 0) {
            pmad_free(buf);
            conn_close(loop_fd, conn->fd, pool);
            return;
        }
    }

    Message msg;
    memset(&msg, 0, sizeof(Message));
    msg.version = hdr.version;
    msg.type    = (MessageType)hdr.type;

    if (payload_len > 0 && buf != NULL) {
        uint8_t* p = buf;

        switch (msg.type) {
            case MSG_SUBMIT: {
                if (payload_len < 1) goto parse_error;
                uint8_t type_len = p[0];
                if (type_len >= PROTOCOL_MAX_TYPE_LEN)
                    goto parse_error;
                if (payload_len < 1u + type_len)
                    goto parse_error;
                memcpy(msg.task_type, p + 1, type_len);
                msg.task_type[type_len] = '\0';
                msg.payload     = p + 1 + type_len;
                msg.payload_len = payload_len - 1 - type_len;
                break;
            }

            case MSG_DONE:
            case MSG_FAILED: {
                if (payload_len < 4) goto parse_error;
                memcpy(&msg.task_id, p, 4);
                msg.task_id = ntohl(msg.task_id);
                break;
            }

            case MSG_STATS_RESPONSE: {
                msg.payload     = p;
                msg.payload_len = payload_len;
                break;
            }

            case MSG_READY:
            case MSG_WAIT:
            case MSG_HEARTBEAT:
            case MSG_PONG:
            case MSG_STATS:
                break;

            default:
                goto parse_error;
        }
    }

    // pass to daemon
    // daemon copies what it needs before we free buf
    // will be implemented afterwards !!!
    daemon_handle_message(conn, &msg, q, pool);

    if (buf) pmad_free(buf);
    return;

parse_error:
    if (buf) pmad_free(buf);
    protocol_send_error(conn->fd,
                        ERR_INVALID_MSG,
                        "parse error");
}

int server_start(int port, Queue* q, WorkerPool* pool) {
    for (int i = 0; i < MAX_CONNECTIONS; i++) {
        connections[i].fd     = -1;
        connections[i].type   = CONN_UNKNOWN;
        connections[i].worker = NULL;
    }

    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) {
        perror("socket");
        return -1;
    }

    set_reuseaddr(server_fd);
    set_nonblocking(server_fd);

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_port        = htons((uint16_t)port);
    addr.sin_addr.s_addr = INADDR_ANY;

    if (bind(server_fd,
             (struct sockaddr*)&addr,
             sizeof(addr)) < 0) {
        perror("bind");
        close(server_fd);
        return -1;
    }

    if (listen(server_fd, 128) < 0) {
        perror("listen");
        close(server_fd);
        return -1;
    }

    printf("quicx listening on port %d [%s]\n",
           port, EL_BACKEND);

    int loop_fd = el_create();
    if (loop_fd < 0) {
        perror("el_create");
        close(server_fd);
        return -1;
    }

    static Connection server_conn;
    server_conn.fd   = server_fd;
    server_conn.type = CONN_UNKNOWN;

    el_add(loop_fd, server_fd, &server_conn);

    // main event loop ───────────────────────────────────────────────────────

    ElEvent events[EL_MAX_EVENTS];

    while (1) {
        int n = el_wait(loop_fd, events, EL_MAX_EVENTS, -1);

        if (n < 0) {
            if (errno == EINTR) continue;  // signal retry
            perror("el_wait");
            break;
        }

        for (int i = 0; i < n; i++) {
            Connection* conn = (Connection*)events[i].ptr;

            if (conn->fd == server_fd) {
                handle_new_connection(loop_fd, server_fd);
            } else {
                handle_readable(loop_fd, conn, q, pool);
            }
        }
    }

    el_destroy(loop_fd);
    close(server_fd);
    return 0;
}