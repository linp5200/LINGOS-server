/**
 * @file    connection_handler.h
 * @brief   连接协议处理器 - App 与 LING OS 主机通信
 * @version LN-B-5.0.0.0
 * @changes 添加 first_packet 字段以支持每个会话独立的首包状态跟踪
 */

#ifndef DAEMON_CONNECTION_HANDLER_H
#define DAEMON_CONNECTION_HANDLER_H

#include <stdint.h>
#include <stddef.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <time.h>

/* ============================================================
 * 常量定义
 * ============================================================ */

#define CONNECTION_MAGIC         0x4C4E4753  /* "LNGS" */
#define CONNECTION_VERSION       0x0001

#define CONNECTION_DEFAULT_PORT  2937
#define CONNECTION_BACKUP_PORT   2938
#define CONNECTION_HARDWARE_PORT 2837

#define CONNECTION_MAX_CLIENTS   64
#define CONNECTION_AUTH_TIMEOUT  300
#define CONNECTION_HEARTBEAT_INTERVAL 30
#define CONNECTION_CODE_EXPIRE   300
#define CONNECTION_MAX_RETRIES   3
#define CONNECTION_BAN_TIME      600

#define CONNECTION_RECV_BUF_SIZE 8192
#define CONNECTION_SEND_BUF_SIZE 8192

/* ============================================================
 * 消息类型 (TLV)
 * ============================================================ */

typedef enum {
    MSG_AUTH_CODE = 0x0001,
    MSG_AUTH_RESPONSE = 0x0002,
    MSG_CONNECTION_CODE = 0x0003,
    MSG_CONNECTION_RESPONSE = 0x0004,
    MSG_COMMAND = 0x0005,
    MSG_COMMAND_RESPONSE = 0x0006,
    MSG_STATUS = 0x0007,
    MSG_HEARTBEAT = 0x0008,
    MSG_HEARTBEAT_ACK = 0x0009,
    MSG_ERROR = 0x000A
} connection_msg_type_t;

/* ============================================================
 * 错误码
 * ============================================================ */

typedef enum {
    ERR_SUCCESS = 0,
    ERR_SERVER_BUSY = 0x4001,
    ERR_AUTH_INVALID = 0x4002,
    ERR_CODE_INVALID = 0x4003,
    ERR_SESSION_EXPIRED = 0x4004,
    ERR_COMMAND_UNKNOWN = 0x4005,
    ERR_PARAM_INVALID = 0x4006,
    ERR_PERMISSION_DENIED = 0x4007,
    ERR_DEVICE_NOT_FOUND = 0x4008,
    ERR_TIMEOUT = 0x4009
} connection_error_t;

/* ============================================================
 * 连接状态
 * ============================================================ */

typedef enum {
    CONN_STATE_IDLE,
    CONN_STATE_AUTH_WAIT,
    CONN_STATE_AUTH_VERIFIED,
    CONN_STATE_CODE_WAIT,
    CONN_STATE_ESTABLISHED,
    CONN_STATE_CLOSED
} connection_state_t;

/* ============================================================
 * 会话结构
 * ============================================================ */

typedef struct connection_session {
    int socket_fd;
    struct sockaddr_in client_addr;
    char client_ip[INET_ADDRSTRLEN];
    uint32_t session_id;
    connection_state_t state;
    char auth_code[32];
    char connection_code[32];
    char token[64];                /* B2: 认证后签发的会话 token（供 2939 数据流通道校验） */
    time_t connected_at;
    time_t last_heartbeat;
    int error_count;
    time_t ban_until;
    uint8_t is_authenticated;
    uint8_t is_active;
    uint8_t first_packet;          /* 每个会话独立跟踪首包状态 */
    struct connection_session *next;
} connection_session_t;

/* ============================================================
 * 配置结构
 * ============================================================ */

typedef struct {
    int primary_port;
    int backup_port;
    int hardware_port;
    int max_clients;
    int auth_timeout;
    int heartbeat_interval;
    int code_expire_seconds;
    int max_retries;
    int ban_time_seconds;
    int enable_backup;
} connection_config_t;

/* ============================================================
 * 函数声明
 * ============================================================ */

int connection_server_start(const connection_config_t *config);
void connection_server_stop(void);
int connection_server_is_running(void);
int connection_verify_token(const char *token);   /* B4: 校验会话 token（供 2939 WS 通道） */
/* 【先生决策】设备绑定 + 吊销（持久 token 安全） */
void connection_bind_device(const char *token, const char *device_id);
int connection_verify_token_device(const char *token, const char *device_id);
void connection_revoke_token(const char *token);
/* 【修复】独立 token 存储（持久——不依赖活动会话） */
void connection_store_token(const char *token, time_t ttl_seconds);
void connection_revoke_token_store(const char *token);
int connection_get_active_sessions(void);
const connection_config_t* connection_get_config(void);

int connection_send_message(uint32_t session_id, connection_msg_type_t type,
                            const uint8_t *payload, uint32_t payload_len);
int connection_broadcast(connection_msg_type_t type,
                         const uint8_t *payload, uint32_t payload_len);

void connection_generate_auth_code(char *out);
void connection_generate_connection_code(char *out);
int connection_verify_auth_code(const char *code);
int connection_verify_connection_code(const char *code);

int connection_load_config(const char *path);
int connection_save_config(const char *path);

#endif /* DAEMON_CONNECTION_HANDLER_H */
/* 【先生设计】令牌系统扩展（no_verify / pending / add / remove） */
int connection_no_verify_check(const char *ip);
int connection_no_verify_set(const char *ip, const char *time_str, int permanent);
int connection_no_verify_remove(const char *ip);
void connection_pending_set(const char *code, const char *ip, int ttl_seconds);
int connection_pending_match(const char *code, const char *ip);
int connection_token_add(const char *token_arg, const char *ip, const char *uid, long ttl_seconds);
int connection_token_remove(const char *token);
