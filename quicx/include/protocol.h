#ifndef PROTOCOL_H
#define PROTOCOL_H

/*
QXP, the Quicx Protocol: the binary format producers, workers and the CLI
use to talk to quicxd. Every message is a 6-byte header (version, type,
length of the body) followed by the body; a payload is at most
PROTOCOL_MAX_PAYLOAD bytes. MSG_SUBMIT_VALUED is a SUBMIT with one byte in
front: the task's value level, 0..15 (15 = critical, never evicted).
*/

/* values for the protocol */
#define PROTOCOL_VERSION        1
#define PROTOCOL_HEADER_SIZE    6
#define PROTOCOL_MAX_PAYLOAD    1024

/* how long a peer may take to deliver the rest of a frame it has started */
#define PROTOCOL_IO_TIMEOUT_MS 1000
#define PROTOCOL_MAX_TYPE_LEN   32
#define PROTOCOL_HEARTBEAT_MS   5000
#define PROTOCOL_TIMEOUT_MS     15000
#define PROTOCOL_MAX_ERROR_MSG  64
#define PROTOCOL_MAX_FAIL_REASON_MSG 64

#include <stdint.h>
#include <stddef.h>

/* 
This is a enum for all possible messages
that could be send from the protocol.
*/
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
    MSG_SUBMIT_VALUED   = 0x0D,   /* [level][typeLen][type][payload]: a SUBMIT with a value level 0..15 */
} MessageType;

/* Protocol error codes */
typedef enum {
    ERR_NONE             = 0x00,
    ERR_QUEUE_FULL       = 0x01,
    ERR_INVALID_MSG      = 0x02,
    ERR_PAYLOAD_TOO_BIG  = 0x03,
    ERR_UNKNOWN_TYPE     = 0x04,
    ERR_VERSION_MISMATCH = 0x05,
} ErrorCode;

/*
Header of the protocol.
Made packed. No padding.
*/
typedef struct __attribute__((packed)) {
    uint8_t     version;
    uint8_t     type;
    uint32_t    length;
} ProtocolHeader;

_Static_assert(sizeof(ProtocolHeader) == 6, "Header must be exactly 6 bytes");

/* Structure for submit messages's body */
typedef struct {
    char     task_type[PROTOCOL_MAX_TYPE_LEN];  /* user defined what that task is for */
    void*    block;                             /* the task's block: Task record, payload right behind it */
    uint8_t* payload;                           /* points into block, NULL when there is no payload */
    uint32_t payload_len;                       /* the length of the payload */
    uint8_t  level;                             /* value level; plain SUBMIT gets QUEUE_LEVEL_DEFAULT */
} SubmitBody;

/* struct for the body of the Ok message */
typedef struct {
    uint32_t task_id;   /* id of the task that is finished */
} OkBody;

/* struct for the body of the Error message */
typedef struct {
    ErrorCode   error_code;                         /* error code when it fails */
    char        error_msg[PROTOCOL_MAX_ERROR_MSG];  /* the error message from the user */
} ErrorBody;

/* struct for the body of the Task message */
typedef struct {
    uint32_t task_id;                               /* identifier for the task */
    char     task_type[PROTOCOL_MAX_TYPE_LEN];      /* user defined what that task is for */
    uint8_t* payload;                               /* read buffer for the payload from the task*/
    uint32_t payload_len;                           /* the length of the payload */
} TaskBody;

/* struct for the body of the Done message */
typedef struct {
    uint32_t task_id;   /* id of the task that is finished */
} DoneBody;

typedef struct {
    uint32_t    task_id;                                /* id of the failed task */
    char        reason[PROTOCOL_MAX_FAIL_REASON_MSG];   /* reason why task failed */
} FailedBody;

/*
Struct that is used to send a message.
For now it is one struct for every type of message.
*/
typedef struct {
    MessageType  type;      /* type of the message */
    uint8_t      version;   /* version of the message */
    union {
        SubmitBody  submit;
        OkBody      ok;
        TaskBody    task;
        ErrorBody   error;
        DoneBody    done;
        FailedBody  failed;
    } as;
} Message;

/*
read exactly n bytes — loops until all received or error
returns n on success, 0 on disconnect, -1 on error
*/
int protocol_read_exact(int fd, void* buf, int n);

/* Functions to send different messages through the protocol */
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
int protocol_send_stats_response(int fd,
                               const void* stats_hdr,
                               uint32_t    stats_hdr_len,
                               const void* classes,
                               uint32_t    classes_len);


#endif