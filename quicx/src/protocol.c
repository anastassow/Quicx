#include "protocol.h"
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/uio.h>

// read-----------------------------------------

int protocol_read_exact(int fd, void* buf, int n) {
    int total = 0;
    uint8_t* ptr = (uint8_t*)buf;

    while (total < n) {
        int r = read(fd, ptr + total, n - total);
        if (r == 0) return 0;
        if (r < 0) return -1;
        total += r;
    }
    return total;
}

int protocol_read_message(int fd, uint8_t* buf,
                          int bufsize, Message* msg) {
    memset(msg, 0, sizeof(Message));

    ProtocolHeader hdr;
    int r = protocol_read_exact(fd, &hdr, PROTOCOL_HEADER_SIZE);
    if (r <= 0) return r;

    if (hdr.version != PROTOCOL_VERSION) {
        protocol_send_error(fd, ERR_VERSION_MISMATCH, "bad version");
        return -1;
    }

    uint32_t payload_len = ntohl(hdr.length);

    if (payload_len > PROTOCOL_MAX_PAYLOAD) {
        protocol_send_error(fd, ERR_PAYLOAD_TOO_BIG, "payload too large");
        return -1;
    }

    if ((int)payload_len > bufsize) return -1;

    if (payload_len > 0) {
        r = protocol_read_exact(fd, buf, (int)payload_len);
        if (r <= 0) return r;
    }

    msg->version = hdr.version;
    msg->type    = (MessageType)hdr.type;
    uint8_t* p   = buf;

    switch (msg->type) {

        case MSG_SUBMIT: {
            // [type_len: 1][type: type_len bytes][payload: rest]
            if (payload_len < 1) return -1;
            uint8_t type_len = p[0];
            if (type_len >= PROTOCOL_MAX_TYPE_LEN) return -1;
            if (payload_len < 1u + type_len) return -1;
            memcpy(msg->task_type, p + 1, type_len);
            msg->task_type[type_len] = '\0';
            msg->payload     = p + 1 + type_len;
            msg->payload_len = payload_len - 1 - type_len;
            break;
        }

        case MSG_OK: {
            // [task_id: 4 bytes]
            if (payload_len < 4) return -1;
            memcpy(&msg->task_id, p, 4);
            msg->task_id = ntohl(msg->task_id);
            break;
        }

        case MSG_ERROR: {
            // [error_code: 1][message: rest]
            if (payload_len < 1) return -1;
            msg->error_code  = (ErrorCode)p[0];
            uint32_t mlen    = payload_len - 1;
            if (mlen >= sizeof(msg->error_msg))
                mlen = sizeof(msg->error_msg) - 1;
            memcpy(msg->error_msg, p + 1, mlen);
            msg->error_msg[mlen] = '\0';
            break;
        }

        case MSG_READY:
        case MSG_WAIT:
        case MSG_HEARTBEAT:
        case MSG_PONG:
        case MSG_STATS:
            // no payload
            break;

        case MSG_DONE:
        case MSG_FAILED: {
            // [task_id: 4 bytes]
            if (payload_len < 4) return -1;
            memcpy(&msg->task_id, p, 4);
            msg->task_id = ntohl(msg->task_id);
            break;
        }

        case MSG_TASK: {
            // [task_id: 4][type_len: 1][type: type_len][payload: rest]
            if (payload_len < 5) return -1;
            memcpy(&msg->task_id, p, 4);
            msg->task_id     = ntohl(msg->task_id);
            uint8_t type_len = p[4];
            if (type_len >= PROTOCOL_MAX_TYPE_LEN) return -1;
            if (payload_len < 5u + type_len) return -1;
            memcpy(msg->task_type, p + 5, type_len);
            msg->task_type[type_len] = '\0';
            msg->payload     = p + 5 + type_len;
            msg->payload_len = payload_len - 5 - type_len;
            break;
        }

        case MSG_STATS_RESPONSE: {
            msg->payload     = p;
            msg->payload_len = payload_len;
            break;
        }

        default:
            return -1;
    }

    return 1;
}

// writev helper ─────────────────────────────────────────────────────────────

// sends header + up to 8 payload parts in one syscall
// no intermediate buffer — no size limit — no copying
static int send_frame_v(int fd, MessageType type, const struct iovec* parts, int part_count) {
    uint32_t total = 0;
    for (int i = 0; i < part_count; i++)
        total += (uint32_t)parts[i].iov_len;

    ProtocolHeader hdr;
    hdr.version = PROTOCOL_VERSION;
    hdr.type    = (uint8_t)type;
    hdr.length  = htonl(total);

    struct iovec iov[9];
    iov[0].iov_base = &hdr;
    iov[0].iov_len  = PROTOCOL_HEADER_SIZE;
    for (int i = 0; i < part_count; i++)
        iov[i + 1] = parts[i];

    int     iov_count = 1 + part_count;
    ssize_t expected  = (ssize_t)(PROTOCOL_HEADER_SIZE + total);

    ssize_t sent = 0;
    while (sent < expected) {
        ssize_t n = writev(fd, iov, iov_count);
        if (n <= 0) return -1;
        sent += n;

        if (sent < expected) {
            ssize_t skip = n;
            for (int i = 0; i < iov_count && skip > 0; i++) {
                if ((ssize_t)iov[i].iov_len <= skip) {
                    skip -= (ssize_t)iov[i].iov_len;
                    iov[i].iov_len  = 0;
                    iov[i].iov_base = NULL;
                } else {
                    iov[i].iov_base =
                        (uint8_t*)iov[i].iov_base + skip;
                    iov[i].iov_len -= (size_t)skip;
                    skip = 0;
                }
            }
        }
    }

    return 0;
}

// send functions ────────────────────────────────────────────────────────────

int protocol_send_submit(int fd, const char* type, const uint8_t* payload, uint32_t payload_len) {
    uint8_t type_len  = (uint8_t)strlen(type);
    uint8_t prefix[1] = { type_len };

    struct iovec parts[3];
    parts[0].iov_base = prefix;
    parts[0].iov_len  = 1;
    parts[1].iov_base = (void*)type;
    parts[1].iov_len  = type_len;
    parts[2].iov_base = (void*)payload;
    parts[2].iov_len  = payload_len;

    int count = (payload && payload_len > 0) ? 3 : 2;
    return send_frame_v(fd, MSG_SUBMIT, parts, count);
}

int protocol_send_ok(int fd, uint32_t task_id) {
    uint32_t net_id = htonl(task_id);

    struct iovec parts[1];
    parts[0].iov_base = &net_id;
    parts[0].iov_len  = 4;

    return send_frame_v(fd, MSG_OK, parts, 1);
}

int protocol_send_error(int fd, ErrorCode code,
                        const char* reason) {
    uint8_t code_byte = (uint8_t)code;

    struct iovec parts[2];
    parts[0].iov_base = &code_byte;
    parts[0].iov_len  = 1;
    parts[1].iov_base = (void*)reason;
    parts[1].iov_len  = strlen(reason);

    return send_frame_v(fd, MSG_ERROR, parts, 2);
}

int protocol_send_ready(int fd) {
    return send_frame_v(fd, MSG_READY, NULL, 0);
}

int protocol_send_done(int fd, uint32_t task_id) {
    uint32_t net_id = htonl(task_id);

    struct iovec parts[1];
    parts[0].iov_base = &net_id;
    parts[0].iov_len  = 4;

    return send_frame_v(fd, MSG_DONE, parts, 1);
}

int protocol_send_failed(int fd, uint32_t task_id,
                         const char* reason) {
    uint32_t net_id = htonl(task_id);

    struct iovec parts[2];
    parts[0].iov_base = &net_id;
    parts[0].iov_len  = 4;
    parts[1].iov_base = (void*)reason;
    parts[1].iov_len  = strlen(reason);

    return send_frame_v(fd, MSG_FAILED, parts, 2);
}

int protocol_send_task(int fd, uint32_t task_id, const char* type, const uint8_t* payload, uint32_t payload_len) {
    uint32_t net_id   = htonl(task_id);
    uint8_t  type_len = (uint8_t)strlen(type);

    uint8_t meta[5];
    memcpy(meta, &net_id, 4);
    meta[4] = type_len;

    struct iovec parts[3];
    parts[0].iov_base = meta;
    parts[0].iov_len  = 5;
    parts[1].iov_base = (void*)type;
    parts[1].iov_len  = type_len;
    parts[2].iov_base = (void*)payload;
    parts[2].iov_len  = payload_len;

    int count = (payload && payload_len > 0) ? 3 : 2;
    return send_frame_v(fd, MSG_TASK, parts, count);
}

int protocol_send_wait(int fd) {
    return send_frame_v(fd, MSG_WAIT, NULL, 0);
}

int protocol_send_heartbeat(int fd) {
    return send_frame_v(fd, MSG_HEARTBEAT, NULL, 0);
}

int protocol_send_pong(int fd) {
    return send_frame_v(fd, MSG_PONG, NULL, 0);
}

int protocol_send_stats_request(int fd) {
    return send_frame_v(fd, MSG_STATS, NULL, 0);
}

int protocol_send_stats_response(int fd, const uint8_t* json, uint32_t len) {
    struct iovec parts[1];
    parts[0].iov_base = (void*)json;
    parts[0].iov_len  = len;

    return send_frame_v(fd, MSG_STATS_RESPONSE, parts, 1);
}