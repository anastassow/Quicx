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
#include <sys/un.h>

#define UNIX_SOCKET_PATH "/tmp/quicx.sock"

/* functions for the sockets */

/* 
Function to set the connection to nonblocking.
return immediately (with EAGAIN/EWOULDBLOCK) 
instead of blocking
*/
static int set_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) return -1;
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

/* 
Function to set the socket with no delay.
Disables Nagle's algorithm.
*/
static void set_nodelay(int fd) {
    int flag = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag));
}

/* sets reuse address */
static void set_reuseaddr(int fd) {
    int flag = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &flag, sizeof(flag));
}

/* connection managment functions */

/* 
Function remoove connection from demon from 
pool and freeing the memory for it.
*/
static void conn_close(int loop_fd, Connection* conn, WorkerPool* pool) {

    daemon_handle_disconnect(conn, pool);

    el_remove(loop_fd, conn->fd);

    close(conn->fd);

    printf("connection closed fd=%d\n", conn->fd);

    pmad_free(conn);
}

/* 
Function that handles a new connection accepting it 
and allocating a connection and adding it to the pool.
*/
static void handle_new_connection(int loop_fd, int server_fd) {
    struct sockaddr_in addr;
    socklen_t addrlen = sizeof(addr);

    int fd = accept(server_fd, (struct sockaddr*)&addr, &addrlen);

    if (fd < 0) {
        perror("accept");
        return;
    }

    Connection* conn = (Connection*)pmad_alloc(sizeof(Connection));
    if (!conn) {
        fprintf(stderr, "out of memory for connection\n");
        close(fd);
        return;
    }

    conn->fd     = fd;
    conn->type   = CONN_UNKNOWN;
    conn->worker = NULL;

    set_nonblocking(fd);
    set_nodelay(fd);

    el_add(loop_fd, fd, conn);

    printf("new connection fd=%d from %s:%d\n",
           fd,
           inet_ntoa(addr.sin_addr),
           ntohs(addr.sin_port));
}

/*
Function that reads the message from the protocol and gives it to the deamon.
Makes nessesary checks and returns a error to the protocol if it fails. 
*/
static void handle_readable(int loop_fd, Connection* conn,
                             Queue* q, WorkerPool* pool) {

    ProtocolHeader hdr;
    int r = protocol_read_exact(conn->fd, &hdr, PROTOCOL_HEADER_SIZE);
    if (r <= 0) {
        conn_close(loop_fd, conn, pool);
        return;
    }

    if (hdr.version != PROTOCOL_VERSION) {
        protocol_send_error(conn->fd, ERR_VERSION_MISMATCH, "bad version");
        conn_close(loop_fd, conn, pool);
        return;
    }

    uint32_t payload_len = ntohl(hdr.length);

    if (payload_len > PROTOCOL_MAX_PAYLOAD) {
        protocol_send_error(conn->fd, ERR_PAYLOAD_TOO_BIG, "payload too large");
        conn_close(loop_fd, conn, pool);
        return;
    }

    Message msg;
    memset(&msg, 0, sizeof(Message));
    msg.version = hdr.version;
    msg.type    = (MessageType)hdr.type;

    if (msg.type == MSG_SUBMIT) {
        uint8_t  type_len_byte;
        uint32_t len_prefix_size = (uint32_t)sizeof(type_len_byte);

        if (payload_len < len_prefix_size) goto parse_error_nobuf;

        if (protocol_read_exact(conn->fd, &type_len_byte, sizeof(type_len_byte)) <= 0) {
            conn_close(loop_fd, conn, pool);
            return;
        }
        if (type_len_byte >= PROTOCOL_MAX_TYPE_LEN)          goto parse_error_nobuf;
        if (payload_len < len_prefix_size + type_len_byte)   goto parse_error_nobuf;

        char task_type[PROTOCOL_MAX_TYPE_LEN];
        if (type_len_byte > 0) {
            if (protocol_read_exact(conn->fd, task_type, type_len_byte) <= 0) {
                conn_close(loop_fd, conn, pool);
                return;
            }
        }
        task_type[type_len_byte] = '\0';

        uint32_t real_len    = payload_len - len_prefix_size - type_len_byte;
        uint8_t* payload_buf = NULL;
        if (real_len > 0) {
            payload_buf = (uint8_t*)pmad_alloc(real_len);
            if (!payload_buf) {
                protocol_send_error(conn->fd, ERR_QUEUE_FULL, "out of memory");
                return;
            }
            if (protocol_read_exact(conn->fd, payload_buf, (int)real_len) <= 0) {
                pmad_free(payload_buf);
                conn_close(loop_fd, conn, pool);
                return;
            }
        }

        memcpy(msg.as.submit.task_type, task_type, type_len_byte + 1);
        msg.as.submit.payload     = payload_buf;
        msg.as.submit.payload_len = real_len;

        daemon_handle_message(conn, &msg, q, pool);
        return;
    }

    switch (msg.type) {

        case MSG_DONE: {
            uint32_t id_size = (uint32_t)sizeof(msg.as.done.task_id);

            if (payload_len != id_size) goto parse_error_nobuf;
            if (protocol_read_exact(conn->fd, &msg.as.done.task_id, sizeof(msg.as.done.task_id)) <= 0) {
                conn_close(loop_fd, conn, pool);
                return;
            }
            msg.as.done.task_id = ntohl(msg.as.done.task_id);
            break;
        }

        case MSG_FAILED: {
            uint32_t id_size = (uint32_t)sizeof(msg.as.failed.task_id);

            if (payload_len < id_size) goto parse_error_nobuf;

            uint32_t reason_len = payload_len - id_size;
            if (reason_len >= PROTOCOL_MAX_FAIL_REASON_MSG) goto parse_error_nobuf;

            if (protocol_read_exact(conn->fd, &msg.as.failed, id_size + reason_len) <= 0) {
                conn_close(loop_fd, conn, pool);
                return;
            }
            msg.as.failed.task_id = ntohl(msg.as.failed.task_id);
            msg.as.failed.reason[reason_len] = '\0';
            break;
        }

        case MSG_READY:
        case MSG_WAIT:
        case MSG_HEARTBEAT:
        case MSG_PONG:
        case MSG_STATS:
            break;

        default: {
            goto parse_error_nobuf;
        }
    }

    daemon_handle_message(conn, &msg, q, pool);
    return;

/* go to for wehen a parsing error occurs */
parse_error_nobuf:
    protocol_send_error(conn->fd, ERR_INVALID_MSG, "parse error");
    conn_close(loop_fd, conn, pool);
}

/* Function for connecting a unix socket for the cli */
static void handle_new_unix_connection(int loop_fd, int unix_fd) {
    int fd = accept(unix_fd, NULL, NULL);
    if (fd < 0) { perror("accept unix"); return; }

    Connection* conn = (Connection*)pmad_alloc(sizeof(Connection));
    if (!conn) { close(fd); return; }

    conn->fd     = fd;
    conn->type   = CONN_UNKNOWN;
    conn->worker = NULL;

    set_nonblocking(fd);
    el_add(loop_fd, fd, conn);

    printf("cli connected fd=%d\n", fd);
}

int server_start(int port, Queue* q, WorkerPool* pool) {

    /* TCP socket */
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

    if (bind(server_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
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

    /* unix socket for CLI */
    int unix_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (unix_fd < 0) {
        perror("unix socket");
        unix_fd = -1;
    } else {
        struct sockaddr_un un;
        memset(&un, 0, sizeof(un));
        un.sun_family = AF_UNIX;
        strncpy(un.sun_path, UNIX_SOCKET_PATH,
                sizeof(un.sun_path) - 1);
        unlink(UNIX_SOCKET_PATH);

        if (bind(unix_fd, (struct sockaddr*)&un, sizeof(un)) < 0) {
            perror("unix bind");
            close(unix_fd);
            unix_fd = -1;
        } else {
            listen(unix_fd, 16);
            printf("quicx cli socket: %s\n", UNIX_SOCKET_PATH);
        }
    }

    int loop_fd = el_create();
    if (loop_fd < 0) {
        perror("el_create");
        close(server_fd);
        return -1;
    }

    /* listening sockets for server */
    static Connection server_conn;
    server_conn.fd     = server_fd;
    server_conn.type   = CONN_UNKNOWN;
    server_conn.worker = NULL;
    el_add(loop_fd, server_fd, &server_conn);

    /* listening sockets for cli */
    static Connection unix_conn;
    if (unix_fd >= 0) {
        unix_conn.fd     = unix_fd;
        unix_conn.type   = CONN_UNKNOWN;
        unix_conn.worker = NULL;
        el_add(loop_fd, unix_fd, &unix_conn);
    }

    /* main event loop */

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
            } else if (unix_fd >= 0 && conn->fd == unix_fd) {
                handle_new_unix_connection(loop_fd, unix_fd);
            } else {
                handle_readable(loop_fd, conn, q, pool);
            }
        }
    }

    el_destroy(loop_fd);
    close(server_fd);
    if (unix_fd >= 0) {
        close(unix_fd);
        unlink(UNIX_SOCKET_PATH);
    }
    return 0;
}