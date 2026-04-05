#ifndef PROTOCOL_H
#define PROTOCOL_H

#define PROTOCOL_VERSION        1
#define PROTOCOL_HEADER_SIZE    6
#define PROTOCOL_MAX_PAYLOAD    (1024 * 1024)
#define PROTOCOL_MAX_TYPE_LEN   32
#define PROTOCOL_HEARTBEAT_MS   5000
#define PROTOCOL_TIMEOUT_MS     15000

#include <stdint.h>
#include <stddef.h>

typedef enum {
    MSG_SUBMIT          = 0x01,
    MSG_OK              = 0x02,
    MSG_ERROR           = 0x03,
    MSG_READY           = 0x04,
    MSG_DONE            = 0x05,
    MSG_FAILED          = 0x06,
    MSG_TASK            = 0x07,
    MSG_WAIT            = 0x08,
    MSG_HEARTBEAT       = 0x09,
    MSG_PONG            = 0x0A,
    MSG_STATS           = 0x0B,
    MSG_STATS_RESPONSE  = 0x0C,
} MessageType;

typedef enum {
    ERR_NONE             = 0x00,
    ERR_QUEUE_FULL       = 0x01,
    ERR_INVALID_MSG      = 0x02,
    ERR_PAYLOAD_TOO_BIG  = 0x03,
    ERR_UNKNOWN_TYPE     = 0x04,
    ERR_VERSION_MISMATCH = 0x05,
} ErrorCode;

typedef struct __attribute__((packed)) {
    uint8_t     version;
    uint8_t     type;
    uint32_t    length;
} ProtocolHeader;

_Static_assert(sizeof(ProtocolHeader) == 6, "Header must be exactly 6 bytes");

typedef struct {
    MessageType  type;
    uint8_t      version;
    ErrorCode    error_code;
    uint32_t     task_id;
    char         task_type[PROTOCOL_MAX_TYPE_LEN];
    uint8_t*     payload; // read buffer only
    uint32_t     payload_len;
    char         error_msg[64];
} Message;


// read / write-------------------------------------------------

// read exactly n bytes — loops until all received or error
// returns n on success, 0 on disconnect, -1 on error
int protocol_read_exact(int fd, void* buf, int n);

// sending--------------------------------------------------------------------

int protocol_send_submit(int fd, const char* type, const uint8_t* payload, uint32_t payload_len);
int protocol_send_ok(int fd, uint32_t task_id);
int protocol_send_error(int fd, ErrorCode code, const char* reason);
int protocol_send_ready(int fd);
int protocol_send_done(int fd, uint32_t task_id);
int protocol_send_failed(int fd, uint32_t task_id, const char* reason);
int protocol_send_task(int fd, uint32_t task_id, const char* type, const uint8_t* payload, uint32_t payload_len);
int protocol_send_wait(int fd);
int protocol_send_heartbeat(int fd);
int protocol_send_pong(int fd);
int protocol_send_stats_request(int fd);
int protocol_send_stats_response(int fd, const uint8_t* json, uint32_t len);


#endif