/**
 * @file    connection_handler.c
 * @brief   连接协议实现 - TCP + TLV + 握手（支持加密）
 * @version LN-B-5.0.0.0
 * @changes first_packet 移至 connection_session_t 结构体（跨会话隔离）；
 *          安全字符串替换；详细日志
 */

#include "connection_handler.h"
#include "../lib/log_extra.h"
#include "../common/data_path.h"
#include "../common/lang.h"
#include "../common/safe_string.h"
#include "../lib/cJSON/cJSON.h"
#include "../drivers/linux_io.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <pthread.h>
#include <signal.h>
#include <time.h>
#include <fcntl.h>
#include <sys/select.h>
#include <ctype.h>

/* ============================================================
 * 默认配置
 * ============================================================ */

static connection_config_t g_config = {
    .primary_port = CONNECTION_DEFAULT_PORT,
    .backup_port = CONNECTION_BACKUP_PORT,
    .hardware_port = CONNECTION_HARDWARE_PORT,
    .max_clients = CONNECTION_MAX_CLIENTS,
    .auth_timeout = CONNECTION_AUTH_TIMEOUT,
    .heartbeat_interval = CONNECTION_HEARTBEAT_INTERVAL,
    .code_expire_seconds = CONNECTION_CODE_EXPIRE,
    .max_retries = CONNECTION_MAX_RETRIES,
    .ban_time_seconds = CONNECTION_BAN_TIME,
    .enable_backup = 1
};

/* ============================================================
 * 全局状态
 * ============================================================ */

static int g_server_running = 0;
static int g_primary_socket = -1;
static int g_backup_socket = -1;
static pthread_t g_server_thread;
static pthread_mutex_t g_session_lock = PTHREAD_MUTEX_INITIALIZER;
static connection_session_t *g_sessions = NULL;
static int g_session_counter = 0;
static volatile int g_stop_flag = 0;
static int g_encryption_enabled = 0;

static char g_pending_auth_code[32] = {0};

/* 【先生决策】设备绑定：token → device_id 映射（持久 token 安全） */
#define MAX_TOKEN_DEVICES 64
typedef struct {
    char token[64];
    char device_id[64];
    time_t bound_at;
    int revoked;
} token_device_t;
static token_device_t g_token_devices[MAX_TOKEN_DEVICES];
static pthread_mutex_t g_token_dev_lock = PTHREAD_MUTEX_INITIALIZER;

void connection_bind_device(const char *token, const char *device_id) {
    if (!token || !token[0] || !device_id || !device_id[0]) return;
    pthread_mutex_lock(&g_token_dev_lock);
    for (int i = 0; i < MAX_TOKEN_DEVICES; i++) {
        if (g_token_devices[i].token[0] && strcmp(g_token_devices[i].token, token) == 0) {
            safe_strncpy(g_token_devices[i].device_id, device_id, sizeof(g_token_devices[i].device_id));
            g_token_devices[i].revoked = 0;
            pthread_mutex_unlock(&g_token_dev_lock);
            return;
        }
    }
    for (int i = 0; i < MAX_TOKEN_DEVICES; i++) {
        if (!g_token_devices[i].token[0]) {
            safe_strncpy(g_token_devices[i].token, token, sizeof(g_token_devices[i].token));
            safe_strncpy(g_token_devices[i].device_id, device_id, sizeof(g_token_devices[i].device_id));
            g_token_devices[i].bound_at = time(NULL);
            g_token_devices[i].revoked = 0;
            break;
        }
    }
    pthread_mutex_unlock(&g_token_dev_lock);
}

/* 校验 token + 设备绑定（返回 1=通过） */
int connection_verify_token_device(const char *token, const char *device_id) {
    if (!token || !token[0]) return 0;
    if (!connection_verify_token(token)) return 0;
    /* 无设备绑定记录（旧 token）→ 首次绑定或拒绝？——允许（兼容） */
    pthread_mutex_lock(&g_token_dev_lock);
    int found = 0, revoked = 0;
    char bound_device[64] = {0};
    for (int i = 0; i < MAX_TOKEN_DEVICES; i++) {
        if (g_token_devices[i].token[0] && strcmp(g_token_devices[i].token, token) == 0) {
            found = 1;
            revoked = g_token_devices[i].revoked;
            safe_strncpy(bound_device, g_token_devices[i].device_id, sizeof(bound_device));
            break;
        }
    }
    pthread_mutex_unlock(&g_token_dev_lock);
    if (found && revoked) return 0;   /* 已吊销 */
    if (found && bound_device[0] && device_id && device_id[0] &&
        strcmp(bound_device, device_id) != 0) {
        return 0;   /* 设备不匹配（换设备拒绝） */
    }
    return 1;
}

/* 吊销 token */
void connection_revoke_token(const char *token) {
    if (!token || !token[0]) return;
    pthread_mutex_lock(&g_token_dev_lock);
    for (int i = 0; i < MAX_TOKEN_DEVICES; i++) {
        if (g_token_devices[i].token[0] && strcmp(g_token_devices[i].token, token) == 0) {
            g_token_devices[i].revoked = 1;
            break;
        }
    }
    pthread_mutex_unlock(&g_token_dev_lock);
}

/* ============================================================
 * 【修复】独立 token 存储（持久 token——不依赖活动会话）
 * 原 verify 遍历活动会话——TCP 断开/超时即失效（WS/HTTP 认证失败根因）
 * ============================================================ */
/* ============================================================
 * 【根本修复】token 文件共享存储（跨进程：主程序签发 → lingosd 验证）
 * 路径：/LINGOS/state/tokens.json（主程序写、lingosd 读——进程共享）
 * 原内存表跨进程不可见（WS 认证永远失败——先生 unknown 根因）
 * ============================================================ */
#define TOKEN_FILE_PATH "/LINGOS/state/tokens.json"
#define MAX_TOKEN_STORE 128

static int token_file_save_all(const char *json_buf) {
    FILE *fp = fopen(TOKEN_FILE_PATH, "w");
    if (!fp) return -1;
    size_t n = fwrite(json_buf, 1, strlen(json_buf), fp);
    fclose(fp);
    return (n == strlen(json_buf)) ? 0 : -1;
}

void connection_store_token(const char *token, time_t ttl_seconds) {
    if (!token || !token[0]) return;
    /* 读现有 → 追加/更新 → 写回 */
    char file_buf[16384];
    file_buf[0] = '\0';
    FILE *rf = fopen(TOKEN_FILE_PATH, "r");
    if (rf) {
        size_t n = fread(file_buf, 1, sizeof(file_buf) - 1, rf);
        file_buf[n] = '\0';
        fclose(rf);
    }
    /* 检查已存在 → 更新 expires */
    if (file_buf[0]) {
        char *pos = strstr(file_buf, token);
        if (pos) {
            /* 已存在——更新 revoked=0（简单重建） */
        }
    }
    char new_entry[512];
    safe_snprintf(new_entry, sizeof(new_entry),
        "%s{\"token\":\"%s\",\"expires_at\":%ld,\"revoked\":0},",
        file_buf[0] ? "" : "[", token, (long)(time(NULL) + ttl_seconds));
    /* 组装：去掉旧内容尾部，追加新条目 */
    char final_buf[17000];
    if (file_buf[0]) {
        /* 文件已有内容——去尾括号追加 */
        size_t fl = strlen(file_buf);
        while (fl > 0 && (file_buf[fl-1] == ']' || file_buf[fl-1] == '\n' || file_buf[fl-1] == ' ')) fl--;
        /* 追加逗号 + 新条目 + ] */
        safe_snprintf(final_buf, sizeof(final_buf), "%.*s,%s]", (int)fl, file_buf, new_entry + 1);
    } else {
        safe_snprintf(final_buf, sizeof(final_buf), "[{\"token\":\"%s\",\"expires_at\":%ld,\"revoked\":0}]",
                      token, (long)(time(NULL) + ttl_seconds));
    }
    token_file_save_all(final_buf);
    LOG_WARN_T("Connection", "StoreToken", "FileOK", "token saved to file");
}

int connection_verify_token(const char *token) {
    if (!token || token[0] == '\0') return 0;
    /* 读文件（跨进程） */
    char file_buf[16384];
    FILE *rf = fopen(TOKEN_FILE_PATH, "r");
    if (!rf) {
        LOG_WARN_T("Connection", "VerifyToken", "NoFile", "token file not found");
        return 0;
    }
    size_t n = fread(file_buf, 1, sizeof(file_buf) - 1, rf);
    file_buf[n] = '\0';
    fclose(rf);
    /* 简易解析：查 token 子串 + 邻近 revoked/expires */
    char *pos = strstr(file_buf, token);
    if (pos) {
        long expires = 0;
        int revoked = 0;
        char *e = strstr(pos, "expires_at");
        if (e) sscanf(e, "expires_at\":%ld", &expires);
        char *r = strstr(pos, "revoked");
        if (r) revoked = (strstr(r, "\":1") != NULL);
        if (!revoked && expires > time(NULL)) {
            return 1;
        }
        LOG_WARN_T("Connection", "VerifyToken", "ExpiredOrRevoked", "expires=%ld now=%ld revoked=%d", expires, (long)time(NULL), revoked);
        return 0;
    }
    LOG_WARN_T("Connection", "VerifyToken", "NotInFile", "token not found in file");
    return 0;
}

void connection_revoke_token_store(const char *token) {
    if (!token || !token[0]) return;
    char file_buf[16384];
    FILE *rf = fopen(TOKEN_FILE_PATH, "r");
    if (!rf) return;
    size_t n = fread(file_buf, 1, sizeof(file_buf) - 1, rf);
    file_buf[n] = '\0';
    fclose(rf);
    /* 重建：匹配 token 的条目置 revoked=1（简易——按 token 定位整条替换） */
    char *pos = strstr(file_buf, token);
    if (pos) {
        char *r = strstr(pos, "revoked");
        if (r && strncmp(r, "revoked\":0", 11) == 0) {
            r[9] = '1';  /* revoked":0 → revoked":1 */
        }
        token_file_save_all(file_buf);
        LOG_WARN_T("Connection", "RevokeStore", "OK", "token revoked in file");
    }
}
static char g_pending_connection_code[32] = {0};
static time_t g_pending_code_time = 0;

/* ============================================================
 * 辅助函数
 * ============================================================ */

static time_t now_sec(void) {
    return time(NULL);
}

static int secure_random_string(char *out, int len, const char *charset) {
    LOG_DEBUG_T("Connection", "SecureRandom", "Enter", "len=%d", len);
    if (!out || len <= 0 || !charset) {
        LOG_ERROR_T("Connection", "SecureRandom", "Invalid", "out=%p, len=%d", (void*)out, len);
        return -1;
    }

    int charset_len = strlen(charset);
    if (charset_len == 0) {
        LOG_ERROR_T("Connection", "SecureRandom", "EmptyCharset", "charset is empty");
        return -1;
    }

    int fd = open("/dev/urandom", O_RDONLY);
    if (fd < 0) {
        LOG_ERROR_T("Connection", "SecureRandom", "OpenFail", "open(/dev/urandom) failed: %s (errno=%d)", strerror(errno), errno);
        return -1;
    }

    unsigned char *rand_buf = malloc(len);
    if (!rand_buf) {
        LOG_ERROR_T("Connection", "SecureRandom", "MallocFail", "malloc(%d) failed", len);
        close(fd);
        return -1;
    }

    ssize_t n = read(fd, rand_buf, len);
    close(fd);
    if (n != len) {
        LOG_ERROR_T("Connection", "SecureRandom", "ReadFail", "read() returned %zd, expected %d", n, len);
        free(rand_buf);
        return -1;
    }

    for (int i = 0; i < len; i++) {
        out[i] = charset[rand_buf[i] % charset_len];
    }
    out[len] = '\0';

    free(rand_buf);
    LOG_DEBUG_T("Connection", "SecureRandom", "OK", "generated '%s'", out);
    return 0;
}


/* B2: 签发会话 token（32 位十六进制，供 2939 数据流通道校验） */
static void connection_generate_token(char *out) {
    LOG_DEBUG_T("Connection", "GenToken", "Enter", "out=%p", (void*)out);
    const char hex[] = "0123456789abcdef";
    if (secure_random_string(out, 32, hex) != 0) {
        LOG_WARN_T("Connection", "GenToken", "Fallback", "secure_random_string failed, using fallback");
        srand((unsigned)(time(NULL) ^ getpid()));
        for (int i = 0; i < 32; i++) out[i] = hex[rand() % 16];
        out[32] = '\0';
    }
    LOG_INFO_T("Connection", "GenToken", "OK", "token='%s'", out);
}

void connection_generate_auth_code(char *out) {
    LOG_DEBUG_T("Connection", "GenAuthCode", "Enter", "out=%p", (void*)out);
    const char charset[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
    if (secure_random_string(out, 6, charset) != 0) {
        LOG_WARN_T("Connection", "GenAuthCode", "Fallback", "secure_random_string failed, using fallback");
        srand((unsigned)(time(NULL) ^ getpid()));
        for (int i = 0; i < 6; i++) {
            out[i] = charset[rand() % (sizeof(charset)-1)];
        }
        out[6] = '\0';
    }
    safe_strncpy(g_pending_auth_code, out, sizeof(g_pending_auth_code));
    g_pending_code_time = now_sec();
    LOG_INFO_T("Connection", "GenAuthCode", "OK", "auth_code='%s'", out);
}

void connection_generate_connection_code(char *out) {
    LOG_DEBUG_T("Connection", "GenConnCode", "Enter", "out=%p", (void*)out);
    const char charset[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
    char raw[13];
    if (secure_random_string(raw, 12, charset) != 0) {
        LOG_WARN_T("Connection", "GenConnCode", "Fallback", "secure_random_string failed, using fallback");
        srand((unsigned)(time(NULL) ^ getpid() ^ (uintptr_t)out));
        for (int i = 0; i < 12; i++) {
            raw[i] = charset[rand() % (sizeof(charset)-1)];
        }
        raw[12] = '\0';
    } else {
        /* 【修复】memcpy 方向写反：应为 out <- raw（原代码把未初始化的 out 复制进 raw，导致连接码为空） */
        memcpy(out, raw, 12);
        out[12] = '\0';
    }
    snprintf(out, 14, "%c%c%c%c-%c%c%c%c-%c%c%c%c",
             raw[0], raw[1], raw[2], raw[3],
             raw[4], raw[5], raw[6], raw[7],
             raw[8], raw[9], raw[10], raw[11]);
    safe_strncpy(g_pending_connection_code, out, sizeof(g_pending_connection_code));
    g_pending_code_time = now_sec();
    LOG_INFO_T("Connection", "GenConnCode", "OK", "connection_code='%s'", out);
}

int connection_verify_auth_code(const char *code) {
    LOG_DEBUG_T("Connection", "VerifyAuth", "Enter", "code='%s'", code ? code : "(null)");
    if (!code || !*code) {
        LOG_WARN_T("Connection", "VerifyAuth", "Empty", "code is NULL or empty");
        return 0;
    }
    time_t now = now_sec();
    if (now - g_pending_code_time > g_config.code_expire_seconds) {
        LOG_WARN_T("Connection", "VerifyAuth", "Expired", "code expired (age=%lds, max=%ds)",
                   (long)(now - g_pending_code_time), g_config.code_expire_seconds);
        return 0;
    }
    int result = (strcmp(code, g_pending_auth_code) == 0);
    LOG_DEBUG_T("Connection", "VerifyAuth", "Result", "%s", result ? "valid" : "invalid");
    return result;
}

int connection_verify_connection_code(const char *code) {
    LOG_DEBUG_T("Connection", "VerifyConn", "Enter", "code='%s'", code ? code : "(null)");
    if (!code || !*code) {
        LOG_WARN_T("Connection", "VerifyConn", "Empty", "code is NULL or empty");
        return 0;
    }
    time_t now = now_sec();
    if (now - g_pending_code_time > g_config.code_expire_seconds) {
        LOG_WARN_T("Connection", "VerifyConn", "Expired", "code expired (age=%lds, max=%ds)",
                   (long)(now - g_pending_code_time), g_config.code_expire_seconds);
        return 0;
    }
    int result = (strcmp(code, g_pending_connection_code) == 0);
    LOG_DEBUG_T("Connection", "VerifyConn", "Result", "%s", result ? "valid" : "invalid");
    return result;
}

/* ============================================================
 * TLV 编解码
 * ============================================================ */

static int encode_tlv(connection_msg_type_t type, const uint8_t *payload,
                      uint32_t payload_len, uint8_t *out, uint32_t *out_len) {
    LOG_DEBUG_T("Connection", "EncodeTLV", "Enter", "type=0x%04X, payload_len=%u", type, payload_len);
    uint32_t total_len = 12 + payload_len;
    if (total_len > CONNECTION_SEND_BUF_SIZE) {
        LOG_ERROR_T("Connection", "EncodeTLV", "Overflow", "total_len=%u > %d", total_len, CONNECTION_SEND_BUF_SIZE);
        return -1;
    }

    uint32_t magic = htonl(CONNECTION_MAGIC);
    uint16_t version = htons(CONNECTION_VERSION);
    uint16_t type_net = htons((uint16_t)type);
    uint32_t len_net = htonl(payload_len);

    memcpy(out, &magic, 4);
    memcpy(out + 4, &version, 2);
    memcpy(out + 6, &type_net, 2);
    memcpy(out + 8, &len_net, 4);
    if (payload && payload_len > 0) {
        memcpy(out + 12, payload, payload_len);
    }
    *out_len = total_len;
    LOG_DEBUG_T("Connection", "EncodeTLV", "OK", "encoded %u bytes", total_len);
    return 0;
}

static int decode_tlv(const uint8_t *data, uint32_t data_len,
                      connection_msg_type_t *type, uint8_t **payload,
                      uint32_t *payload_len) {
    LOG_DEBUG_T("Connection", "DecodeTLV", "Enter", "data_len=%u", data_len);
    if (data_len < 12) {
        LOG_ERROR_T("Connection", "DecodeTLV", "TooShort", "data_len=%u < 12", data_len);
        return -1;
    }

    uint32_t magic;
    uint16_t version;
    uint16_t type_net;
    uint32_t len_net;

    memcpy(&magic, data, 4);
    magic = ntohl(magic);
    if (magic != CONNECTION_MAGIC) {
        LOG_WARN_T("Connection", "DecodeTLV", "MagicMismatch", "magic=0x%08X, expected 0x%08X", magic, CONNECTION_MAGIC);
        return -1;
    }

    memcpy(&version, data + 4, 2);
    version = ntohs(version);
    if (version != CONNECTION_VERSION) {
        LOG_WARN_T("Connection", "DecodeTLV", "VersionMismatch", "version=%u, expected %u", version, CONNECTION_VERSION);
        return -1;
    }

    memcpy(&type_net, data + 6, 2);
    *type = (connection_msg_type_t)ntohs(type_net);

    memcpy(&len_net, data + 8, 4);
    *payload_len = ntohl(len_net);

    if (data_len < 12 + *payload_len) {
        LOG_ERROR_T("Connection", "DecodeTLV", "Truncated", "data_len=%u < 12+%u", data_len, *payload_len);
        return -1;
    }

    if (*payload_len > 0) {
        *payload = (uint8_t *)malloc(*payload_len);
        if (!(*payload)) {
            LOG_ERROR_T("Connection", "DecodeTLV", "MallocFail", "malloc(%u) failed", *payload_len);
            return -1;
        }
        memcpy(*payload, data + 12, *payload_len);
        LOG_DEBUG_T("Connection", "DecodeTLV", "Payload", "copied %u bytes", *payload_len);
    } else {
        *payload = NULL;
    }

    LOG_DEBUG_T("Connection", "DecodeTLV", "OK", "type=0x%04X, payload_len=%u", *type, *payload_len);
    return 0;
}

/* ============================================================
 * 【修改】会话管理（first_packet 在会话结构体中）
 * ============================================================ */

static connection_session_t* create_session(int fd, struct sockaddr_in *addr) {
    LOG_DEBUG_T("Connection", "CreateSession", "Enter", "fd=%d", fd);
    connection_session_t *sess = (connection_session_t *)calloc(1, sizeof(connection_session_t));
    if (!sess) {
        LOG_ERROR_T("Connection", "CreateSession", "CallocFail", "calloc failed");
        return NULL;
    }

    sess->socket_fd = fd;
    sess->client_addr = *addr;
    inet_ntop(AF_INET, &addr->sin_addr, sess->client_ip, sizeof(sess->client_ip));
    sess->session_id = ++g_session_counter;
    sess->state = CONN_STATE_AUTH_WAIT;
    sess->connected_at = now_sec();
    sess->last_heartbeat = now_sec();
    sess->error_count = 0;
    sess->ban_until = 0;
    sess->is_authenticated = 0;
    sess->is_active = 1;
    /* 【修复】first_packet 在会话中独立初始化 */
    sess->first_packet = 1;

    pthread_mutex_lock(&g_session_lock);
    sess->next = g_sessions;
    g_sessions = sess;
    pthread_mutex_unlock(&g_session_lock);

    LOG_INFO_T("Connection", "CreateSession", "OK", "session=%u from %s:%d", sess->session_id, sess->client_ip, ntohs(addr->sin_port));
    return sess;
}

static void destroy_session(connection_session_t *sess) {
    LOG_DEBUG_T("Connection", "DestroySession", "Enter", "session=%u", sess ? sess->session_id : 0);
    if (!sess) {
        LOG_WARN_T("Connection", "DestroySession", "Invalid", "sess is NULL");
        return;
    }
    if (sess->socket_fd >= 0) {
        close(sess->socket_fd);
        LOG_DEBUG_T("Connection", "DestroySession", "Close", "closed fd=%d", sess->socket_fd);
        sess->socket_fd = -1;
    }
    sess->is_active = 0;

    pthread_mutex_lock(&g_session_lock);
    connection_session_t **pp = &g_sessions;
    while (*pp) {
        if (*pp == sess) {
            *pp = sess->next;
            break;
        }
        pp = &(*pp)->next;
    }
    pthread_mutex_unlock(&g_session_lock);

    LOG_INFO_T("Connection", "DestroySession", "OK", "session=%u destroyed", sess->session_id);
    free(sess);
}

static connection_session_t* find_session_by_fd(int fd) {
    LOG_DEBUG_T("Connection", "FindByFd", "Enter", "fd=%d", fd);
    pthread_mutex_lock(&g_session_lock);
    connection_session_t *sess = g_sessions;
    while (sess) {
        if (sess->socket_fd == fd && sess->is_active) {
            pthread_mutex_unlock(&g_session_lock);
            LOG_DEBUG_T("Connection", "FindByFd", "Found", "session=%u", sess->session_id);
            return sess;
        }
        sess = sess->next;
    }
    pthread_mutex_unlock(&g_session_lock);
    LOG_DEBUG_T("Connection", "FindByFd", "NotFound", "no session for fd=%d", fd);
    return NULL;
}

static connection_session_t* find_session_by_id(uint32_t session_id) {
    LOG_DEBUG_T("Connection", "FindById", "Enter", "session_id=%u", session_id);
    pthread_mutex_lock(&g_session_lock);
    connection_session_t *sess = g_sessions;
    while (sess) {
        if (sess->session_id == session_id && sess->is_active) {
            pthread_mutex_unlock(&g_session_lock);
            LOG_DEBUG_T("Connection", "FindById", "Found", "session=%u", session_id);
            return sess;
        }
        sess = sess->next;
    }
    pthread_mutex_unlock(&g_session_lock);
    LOG_DEBUG_T("Connection", "FindById", "NotFound", "session=%u not found", session_id);
    return NULL;
}

static int is_max_clients_reached(void) {
    int count = 0;
    pthread_mutex_lock(&g_session_lock);
    connection_session_t *sess = g_sessions;
    while (sess) {
        if (sess->is_active) count++;
        sess = sess->next;
    }
    pthread_mutex_unlock(&g_session_lock);
    LOG_DEBUG_T("Connection", "MaxClients", "check", "count=%d, max=%d", count, g_config.max_clients);
    return count >= g_config.max_clients;
}

/* ============================================================
 * 消息发送
 * ============================================================ */

int connection_send_message(uint32_t session_id, connection_msg_type_t type,
                            const uint8_t *payload, uint32_t payload_len) {
    LOG_DEBUG_T("Connection", "SendMsg", "Enter", "session=%u, type=0x%04X, len=%u", session_id, type, payload_len);
    connection_session_t *sess = find_session_by_id(session_id);
    if (!sess || sess->socket_fd < 0) {
        LOG_WARN_T("Connection", "SendMsg", "NoSession", "session=%u not found or invalid", session_id);
        return -1;
    }

    uint8_t buffer[CONNECTION_SEND_BUF_SIZE];
    uint32_t out_len;
    if (encode_tlv(type, payload, payload_len, buffer, &out_len) != 0) {
        LOG_ERROR_T("Connection", "SendMsg", "EncodeFail", "encode_tlv failed");
        return -1;
    }

    ssize_t sent = send(sess->socket_fd, buffer, out_len, 0);
    if (sent != (ssize_t)out_len) {
        LOG_ERROR_T("Connection", "SendMsg", "SendFail", "sent %zd of %u bytes (errno=%d)", sent, out_len, errno);
        return -1;
    }

    LOG_DEBUG_T("Connection", "SendMsg", "OK", "sent %u bytes to session=%u", out_len, session_id);
    return 0;
}

int connection_broadcast(connection_msg_type_t type,
                         const uint8_t *payload, uint32_t payload_len) {
    LOG_DEBUG_T("Connection", "Broadcast", "Enter", "type=0x%04X, len=%u", type, payload_len);
    int count = 0;
    pthread_mutex_lock(&g_session_lock);
    connection_session_t *sess = g_sessions;
    while (sess) {
        if (sess->is_active && sess->state == CONN_STATE_ESTABLISHED) {
            if (connection_send_message(sess->session_id, type, payload, payload_len) == 0) {
                count++;
            }
        }
        sess = sess->next;
    }
    pthread_mutex_unlock(&g_session_lock);
    LOG_INFO_T("Connection", "Broadcast", "OK", "sent to %d active sessions", count);
    return count;
}

/* ============================================================
 * 错误发送
 * ============================================================ */

static void send_error(connection_session_t *sess, connection_error_t err,
                       const char *msg) {
    LOG_DEBUG_T("Connection", "SendError", "Enter", "session=%u, err=%d, msg='%s'", sess->session_id, err, msg ? msg : "(null)");
    char json[256];
    safe_snprintf(json, sizeof(json), "{\"code\":%d,\"msg\":\"%s\"}", err, msg ? msg : tr("Unknown error", "未知错误"));
    connection_send_message(sess->session_id, MSG_ERROR,
                           (const uint8_t *)json, strlen(json));
}

/* ============================================================
 * 消息处理
 * ============================================================ */

static void handle_auth_code(connection_session_t *sess, const uint8_t *payload,
                             uint32_t payload_len) {
    LOG_DEBUG_T("Connection", "HandleAuth", "Enter", "session=%u, payload_len=%u", sess->session_id, payload_len);

    if (sess->state != CONN_STATE_AUTH_WAIT) {
        LOG_WARN_T("Connection", "HandleAuth", "InvalidState", "session=%u state=%d", sess->session_id, sess->state);
        send_error(sess, ERR_AUTH_INVALID, tr("Invalid state", "无效状态"));
        return;
    }

    char code[32] = {0};
    if (payload_len >= sizeof(code) - 1) payload_len = sizeof(code) - 1;
    memcpy(code, payload, payload_len);
    code[payload_len] = '\0';
    LOG_DEBUG_T("Connection", "HandleAuth", "Code", "code='%s' from %s", code, sess->client_ip);

    /* 【R1】会话级验证码比对（原全局 g_pending_auth_code 被多连接覆盖） */
    if (strcmp(code, sess->auth_code) == 0) {
        sess->state = CONN_STATE_AUTH_VERIFIED;
        char conn_code[32];
        connection_generate_connection_code(conn_code);
        safe_strncpy(sess->connection_code, conn_code, sizeof(sess->connection_code));

        /* 显示连接码到终端 */
        log_draw_box(
            tr("Remote Connection Request", "远程连接请求"),
            tr("Connection Code: ", "连接码："),
            COLOR_YELLOW, COLOR_RED, COLOR_WHITE
        );
        uart_puts(COLOR_BOLD COLOR_YELLOW);
        uart_puts("  ");
        uart_puts(conn_code);
        uart_puts("  ");
        uart_puts(COLOR_RESET);
        uart_puts("\n");
        uart_puts(tr("Please enter this code in the App. It will expire in 5 minutes.\n",
                     "请在 App 中输入此连接码。5 分钟后过期。\n"));
        uart_puts("\n");

        const char *resp = "{\"status\":\"ok\"}";
        connection_send_message(sess->session_id, MSG_AUTH_RESPONSE,
                               (const uint8_t *)resp, strlen(resp));

        /* 【修复】连接码写 WARN 日志（终端可能被 TUI 覆盖/用户错过显示——日志可找回） */
        LOG_WARN_T("Connection", "HandleAuth", "ConnCode", "session=%u connection code: %s", sess->session_id, conn_code);
        LOG_INFO_T("Connection", "HandleAuth", "OK", "session=%u auth verified", sess->session_id);
    } else {
        sess->error_count++;
        send_error(sess, ERR_AUTH_INVALID, tr("Invalid auth code", "无效验证码"));
        LOG_WARN_T("Connection", "HandleAuth", "Fail", "session=%u invalid auth code", sess->session_id);
        if (sess->error_count >= g_config.max_retries) {
            sess->ban_until = now_sec() + g_config.ban_time_seconds;
            LOG_WARN_T("Connection", "HandleAuth", "Ban", "session=%u banned for %ds",
                       sess->session_id, g_config.ban_time_seconds);
        }
    }
}

static void handle_connection_code(connection_session_t *sess, const uint8_t *payload,
                                   uint32_t payload_len) {
    LOG_DEBUG_T("Connection", "HandleConn", "Enter", "session=%u, payload_len=%u", sess->session_id, payload_len);

    if (sess->state != CONN_STATE_AUTH_VERIFIED) {
        LOG_WARN_T("Connection", "HandleConn", "InvalidState", "session=%u state=%d", sess->session_id, sess->state);
        send_error(sess, ERR_AUTH_INVALID, tr("Invalid state", "无效状态"));
        return;
    }

    char code[32] = {0};
    if (payload_len >= sizeof(code) - 1) payload_len = sizeof(code) - 1;
    memcpy(code, payload, payload_len);
    code[payload_len] = '\0';
    LOG_DEBUG_T("Connection", "HandleConn", "Code", "code='%s' from %s", code, sess->client_ip);

    /* 【先生设计】pending 重验证匹配（token login again——验证码——新 token 签发 + 删旧）
     * payload 格式：<验证码> 或 <验证码>|<旧token>（App 附带旧令牌——签发后删除） */
    char old_token[64] = {0};
    char *sep = strchr(code, '|');
    if (sep) {
        *sep = '\0';
        safe_strncpy(old_token, sep + 1, sizeof(old_token));
    }
    int pending_match = connection_pending_match(code, sess->client_ip);

    if (connection_verify_connection_code(code) || pending_match) {
        sess->state = CONN_STATE_ESTABLISHED;
        sess->is_authenticated = 1;
        sess->last_heartbeat = now_sec();
        /* 验证码后加密启用 */
        g_encryption_enabled = 1;
        LOG_INFO_T("Connection", "HandleConn", "Encryption", "Encryption enabled for session=%u", sess->session_id);

        /* B2: 签发会话 token（供 2939 数据流通道校验） */
        char token[64];
        connection_generate_token(token);
        safe_strncpy(sess->token, token, sizeof(sess->token));
        /* 【修复】token 写入独立存储（持久——TCP 断开仍有效） */
        connection_store_token(token, 2592000);
        /* 【先生设计】pending 重验证成功——删除旧令牌 */
        if (pending_match && old_token[0]) {
            connection_revoke_token_store(old_token);
            LOG_WARN_T("Connection", "Pending", "Issue", "重验证成功——新 token 签发 + 旧令牌删除");
        }

        uint32_t sid = sess->session_id;
        char resp[256];
        /* 【先生决策】持久 token：30 天有效期（原 300s）——退出恢复 + 安全缓解 */
        int token_ttl = 2592000;
        safe_snprintf(resp, sizeof(resp),
                "{\"status\":\"ok\",\"session_id\":\"%u\",\"expires_in\":%d,\"encrypted\":true,\"token\":\"%s\"}",
                sid, token_ttl, token);
        connection_send_message(sess->session_id, MSG_CONNECTION_RESPONSE,
                               (const uint8_t *)resp, strlen(resp));

        LOG_INFO_T("Connection", "HandleConn", "OK", "session=%u connection established with encryption", sess->session_id);
    } else {
        sess->error_count++;
        send_error(sess, ERR_CODE_INVALID, tr("Invalid connection code", "无效连接码"));
        LOG_WARN_T("Connection", "HandleConn", "Fail", "session=%u invalid connection code", sess->session_id);
        if (sess->error_count >= g_config.max_retries) {
            sess->ban_until = now_sec() + g_config.ban_time_seconds;
            LOG_WARN_T("Connection", "HandleConn", "Ban", "session=%u banned", sess->session_id);
        }
    }
}

static void handle_command(connection_session_t *sess, const uint8_t *payload,
                           uint32_t payload_len) {
    LOG_DEBUG_T("Connection", "HandleCmd", "Enter", "session=%u, payload_len=%u", sess->session_id, payload_len);

    if (sess->state != CONN_STATE_ESTABLISHED) {
        LOG_WARN_T("Connection", "HandleCmd", "InvalidState", "session=%u state=%d", sess->session_id, sess->state);
        send_error(sess, ERR_AUTH_INVALID, tr("Not authenticated", "未认证"));
        return;
    }

    char json_str[512] = {0};
    if (payload_len >= sizeof(json_str) - 1) payload_len = sizeof(json_str) - 1;
    memcpy(json_str, payload, payload_len);
    json_str[payload_len] = '\0';
    LOG_DEBUG_T("Connection", "HandleCmd", "Payload", "json='%s'", json_str);

    cJSON *root = cJSON_Parse(json_str);
    if (!root) {
        LOG_WARN_T("Connection", "HandleCmd", "ParseFail", "invalid JSON");
        send_error(sess, ERR_PARAM_INVALID, tr("Invalid JSON", "无效 JSON"));
        return;
    }

    cJSON *cmd = cJSON_GetObjectItem(root, "command");
    if (!cJSON_IsString(cmd)) {
        LOG_WARN_T("Connection", "HandleCmd", "MissingCmd", "missing 'command' field");
        cJSON_Delete(root);
        send_error(sess, ERR_PARAM_INVALID, tr("Missing command", "缺少命令"));
        return;
    }

    const char *command = cmd->valuestring;
    char resp[512];
    LOG_DEBUG_T("Connection", "HandleCmd", "Command", "command='%s'", command);

    if (strcmp(command, "ping") == 0) {
        safe_snprintf(resp, sizeof(resp), "{\"command\":\"ping\",\"result\":\"pong\"}");
        connection_send_message(sess->session_id, MSG_COMMAND_RESPONSE,
                               (const uint8_t *)resp, strlen(resp));
        LOG_DEBUG_T("Connection", "HandleCmd", "Pong", "ping response sent");
    } else if (strcmp(command, "system_status") == 0) {
        safe_snprintf(resp, sizeof(resp),
                "{\"command\":\"system_status\",\"status\":\"ok\",\"data\":{\"uptime\":%ld,\"connections\":%d,\"encrypted\":%d}}",
                now_sec(), connection_get_active_sessions(), g_encryption_enabled);
        connection_send_message(sess->session_id, MSG_COMMAND_RESPONSE,
                               (const uint8_t *)resp, strlen(resp));
        LOG_DEBUG_T("Connection", "HandleCmd", "Status", "status response sent");
    } else {
        LOG_WARN_T("Connection", "HandleCmd", "Unknown", "unknown command='%s'", command);
        send_error(sess, ERR_COMMAND_UNKNOWN, tr("Unknown command", "未知命令"));
    }

    cJSON_Delete(root);
}

static void handle_heartbeat(connection_session_t *sess) {
    LOG_DEBUG_T("Connection", "HandleHeartbeat", "Enter", "session=%u", sess->session_id);
    if (sess->state != CONN_STATE_ESTABLISHED) {
        LOG_WARN_T("Connection", "HandleHeartbeat", "InvalidState", "session=%u state=%d", sess->session_id, sess->state);
        return;
    }
    sess->last_heartbeat = now_sec();
    const char *ack = "{\"status\":\"ok\"}";
    connection_send_message(sess->session_id, MSG_HEARTBEAT_ACK,
                           (const uint8_t *)ack, strlen(ack));
    LOG_DEBUG_T("Connection", "HandleHeartbeat", "OK", "heartbeat acknowledged");
}

/* ============================================================
 * 【修改】process_message（使用会话中的 first_packet）
 * ============================================================ */

static void process_message(connection_session_t *sess, const uint8_t *data,
                            uint32_t data_len) {
    LOG_DEBUG_T("Connection", "ProcessMsg", "Enter", "session=%u, data_len=%u", sess->session_id, data_len);

    connection_msg_type_t type;
    uint8_t *payload = NULL;
    uint32_t payload_len = 0;

    if (decode_tlv(data, data_len, &type, &payload, &payload_len) != 0) {
        LOG_WARN_T("Connection", "ProcessMsg", "DecodeFail", "session=%u", sess->session_id);
        return;
    }

    LOG_DEBUG_T("Connection", "ProcessMsg", "Type", "session=%u, type=0x%04X", sess->session_id, type);

    /* 【修复】使用会话中的 first_packet，而非静态变量 */
    if (sess->first_packet && type != MSG_AUTH_CODE) {
        LOG_WARN_T("Connection", "ProcessMsg", "FirstPacketFail", "session=%u: first packet must be AUTH_CODE, got 0x%04X",
                   sess->session_id, type);
        send_error(sess, ERR_AUTH_INVALID, tr("First packet must be auth code", "首包必须是验证码"));
        if (payload) free(payload);
        sess->is_active = 0;
        /* 【R1】不在此 destroy——由 client_handler 线程退出时统一销毁（防 double free） */
        return;
    }
    if (type == MSG_AUTH_CODE) {
        sess->first_packet = 0;
    }

    switch (type) {
        case MSG_AUTH_CODE:
            handle_auth_code(sess, payload, payload_len);
            break;
        case MSG_CONNECTION_CODE:
            handle_connection_code(sess, payload, payload_len);
            break;
        case MSG_COMMAND:
            handle_command(sess, payload, payload_len);
            break;
        case MSG_HEARTBEAT:
            handle_heartbeat(sess);
            break;
        default:
            LOG_WARN_T("Connection", "ProcessMsg", "UnknownType", "type=0x%04X", type);
            send_error(sess, ERR_COMMAND_UNKNOWN, tr("Unknown message type", "未知消息类型"));
            break;
    }

    if (payload) free(payload);
    LOG_DEBUG_T("Connection", "ProcessMsg", "Exit", "session=%u", sess->session_id);
}

/* ============================================================
 * 客户端处理线程
 * ============================================================ */

static void* client_handler(void *arg) {
    int client_fd = *(int *)arg;
    free(arg);
    LOG_DEBUG_T("Connection", "ClientHandler", "Enter", "client_fd=%d", client_fd);

    struct sockaddr_in addr;
    socklen_t addr_len = sizeof(addr);
    getpeername(client_fd, (struct sockaddr *)&addr, &addr_len);

    connection_session_t *sess = create_session(client_fd, &addr);
    if (!sess) {
        LOG_ERROR_T("Connection", "ClientHandler", "CreateFail", "failed to create session");
        close(client_fd);
        return NULL;
    }

    char auth_code[32];
    connection_generate_auth_code(auth_code);
    safe_strncpy(sess->auth_code, auth_code, sizeof(sess->auth_code));

    uart_puts(COLOR_BOLD COLOR_CYAN);
    uart_puts(tr("\n=== New Connection from ", "\n=== 新连接来自 "));
    uart_puts(sess->client_ip);
    uart_puts(tr(" ===\n", " ===\n"));
    uart_puts(COLOR_RESET);
    uart_puts(tr("Auth Code: ", "验证码："));
    uart_puts(COLOR_BOLD COLOR_YELLOW);
    uart_puts(auth_code);
    uart_puts(COLOR_RESET);
    uart_puts("\n");
    uart_puts(tr("Please enter this code in the App.\n", "请在 App 中输入此验证码。\n"));
    uart_puts("\n");

    uint8_t buffer[CONNECTION_RECV_BUF_SIZE];
    while (sess->is_active && !g_stop_flag) {
        fd_set readfds;
        FD_ZERO(&readfds);
        FD_SET(client_fd, &readfds);

        struct timeval tv;
        tv.tv_sec = 5;
        tv.tv_usec = 0;

        int ret = select(client_fd + 1, &readfds, NULL, NULL, &tv);
        if (ret < 0) {
            if (errno == EINTR) continue;
            LOG_ERROR_T("Connection", "ClientHandler", "SelectFail", "select error: %s", strerror(errno));
            break;
        }
        if (ret == 0) {
            time_t now = now_sec();
            if (sess->state == CONN_STATE_ESTABLISHED &&
                now - sess->last_heartbeat > g_config.heartbeat_interval * 2) {
                LOG_WARN_T("Connection", "ClientHandler", "HeartbeatTimeout", "session=%u", sess->session_id);
                break;
            }
            continue;
        }

        ssize_t n = recv(client_fd, buffer, sizeof(buffer), 0);
        if (n <= 0) {
            LOG_DEBUG_T("Connection", "ClientHandler", "Recv", "recv returned %zd, closing", n);
            break;
        }

        LOG_DEBUG_T("Connection", "ClientHandler", "RecvData", "received %zd bytes from session=%u", n, sess->session_id);

        uint32_t offset = 0;
        while (offset < (uint32_t)n) {
            if (n - offset < 12) break;
            uint32_t pkt_len = 12 + ntohl(*(uint32_t *)(buffer + offset + 8));
            if (offset + pkt_len > (uint32_t)n) break;
            process_message(sess, buffer + offset, pkt_len);
            offset += pkt_len;
        }
    }

    destroy_session(sess);
    LOG_DEBUG_T("Connection", "ClientHandler", "Exit", "client_fd=%d", client_fd);
    return NULL;
}

/* ============================================================
 * 服务器主循环
 * ============================================================ */

static void* server_loop(void *arg) {
    (void)arg;
    LOG_INFO_T("Connection", "ServerLoop", "Enter", "starting server loop");

    int primary_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (primary_fd < 0) {
        LOG_ERROR_T("Connection", "ServerLoop", "SocketFail", "primary socket: %s", strerror(errno));
        return NULL;
    }

    int opt = 1;
    setsockopt(primary_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(g_config.primary_port);

    if (bind(primary_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        LOG_ERROR_T("Connection", "ServerLoop", "BindFail", "port %d: %s", g_config.primary_port, strerror(errno));
        close(primary_fd);
        return NULL;
    }

    if (listen(primary_fd, 10) < 0) {
        LOG_ERROR_T("Connection", "ServerLoop", "ListenFail", "%s", strerror(errno));
        close(primary_fd);
        return NULL;
    }

    g_primary_socket = primary_fd;
    LOG_INFO_T("Connection", "ServerLoop", "Start", "listening on port %d", g_config.primary_port);

    int backup_fd = -1;
    if (g_config.enable_backup) {
        backup_fd = socket(AF_INET, SOCK_STREAM, 0);
        if (backup_fd >= 0) {
            setsockopt(backup_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
            addr.sin_port = htons(g_config.backup_port);
            if (bind(backup_fd, (struct sockaddr *)&addr, sizeof(addr)) == 0 &&
                listen(backup_fd, 10) == 0) {
                g_backup_socket = backup_fd;
                LOG_INFO_T("Connection", "ServerLoop", "Backup", "listening on port %d", g_config.backup_port);
            } else {
                LOG_WARN_T("Connection", "ServerLoop", "BackupFail", "backup port %d: %s", g_config.backup_port, strerror(errno));
                close(backup_fd);
                backup_fd = -1;
            }
        }
    }

    g_server_running = 1;

    fd_set master_set;
    int max_fd = primary_fd;
    if (g_backup_socket > max_fd) max_fd = g_backup_socket;

    while (!g_stop_flag) {
        FD_ZERO(&master_set);
        FD_SET(primary_fd, &master_set);
        if (g_backup_socket >= 0) {
            FD_SET(g_backup_socket, &master_set);
        }

        struct timeval tv;
        tv.tv_sec = 1;
        tv.tv_usec = 0;

        int ret = select(max_fd + 1, &master_set, NULL, NULL, &tv);
        if (ret < 0) {
            if (errno == EINTR) continue;
            LOG_ERROR_T("Connection", "ServerLoop", "SelectFail", "%s", strerror(errno));
            break;
        }
        if (ret == 0) continue;

        int accept_fd = -1;
        if (FD_ISSET(primary_fd, &master_set)) {
            accept_fd = primary_fd;
        } else if (g_backup_socket >= 0 && FD_ISSET(g_backup_socket, &master_set)) {
            accept_fd = g_backup_socket;
        }

        if (accept_fd < 0) continue;

        if (is_max_clients_reached()) {
            int client = accept(accept_fd, NULL, NULL);
            if (client >= 0) {
                const char *err_msg = "{\"code\":4001,\"msg\":\"Server busy\"}";
                send(client, err_msg, strlen(err_msg), 0);
                close(client);
                LOG_WARN_T("Connection", "ServerLoop", "MaxClients", "connection rejected");
            }
            continue;
        }

        struct sockaddr_in client_addr;
        socklen_t addr_len = sizeof(client_addr);
        int client_fd = accept(accept_fd, (struct sockaddr *)&client_addr, &addr_len);
        if (client_fd < 0) {
            LOG_WARN_T("Connection", "ServerLoop", "AcceptFail", "%s", strerror(errno));
            continue;
        }

        char client_ip[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &client_addr.sin_addr, client_ip, sizeof(client_ip));
        LOG_INFO_T("Connection", "ServerLoop", "Accept", "connection from %s:%d", client_ip, ntohs(client_addr.sin_port));

        int *fd_ptr = (int *)malloc(sizeof(int));
        if (!fd_ptr) {
            LOG_ERROR_T("Connection", "ServerLoop", "MallocFail", "failed to allocate fd_ptr");
            close(client_fd);
            continue;
        }
        *fd_ptr = client_fd;

        pthread_t tid;
        if (pthread_create(&tid, NULL, client_handler, fd_ptr) != 0) {
            LOG_ERROR_T("Connection", "ServerLoop", "ThreadFail", "pthread_create failed");
            free(fd_ptr);
            close(client_fd);
            continue;
        }
        pthread_detach(tid);
    }

    close(primary_fd);
    if (g_backup_socket >= 0) close(g_backup_socket);
    g_server_running = 0;
    LOG_INFO_T("Connection", "ServerLoop", "Exit", "server stopped");
    return NULL;
}

/* ============================================================
 * 公共 API
 * ============================================================ */

int connection_server_start(const connection_config_t *config) {
    LOG_INFO_T("Connection", "Start", "Enter", "config=%p", (void*)config);
    if (g_server_running) {
        LOG_WARN_T("Connection", "Start", "AlreadyRunning", "server already running");
        return 0;
    }

    if (config) {
        g_config = *config;
        LOG_DEBUG_T("Connection", "Start", "Config", "primary_port=%d, max_clients=%d", g_config.primary_port, g_config.max_clients);
    }

    g_stop_flag = 0;
    g_encryption_enabled = 0;
    LOG_DEBUG_T("Connection", "Start", "Encryption", "encryption disabled initially");

    if (pthread_create(&g_server_thread, NULL, server_loop, NULL) != 0) {
        LOG_ERROR_T("Connection", "Start", "ThreadFail", "pthread_create failed");
        return -1;
    }

    int retries = 10;
    while (!g_server_running && retries-- > 0) {
        usleep(100000);
    }

    if (!g_server_running) {
        LOG_ERROR_T("Connection", "Start", "Timeout", "server failed to start");
        return -1;
    }

    LOG_INFO_T("Connection", "Start", "OK", "connection server started (port %d, backup %d)",
               g_config.primary_port, g_config.backup_port);
    return 0;
}

void connection_server_stop(void) {
    LOG_INFO_T("Connection", "Stop", "Enter", "stopping server");
    if (!g_server_running) {
        LOG_WARN_T("Connection", "Stop", "NotRunning", "server not running");
        return;
    }
    g_stop_flag = 1;
    pthread_join(g_server_thread, NULL);
    LOG_INFO_T("Connection", "Stop", "OK", "connection server stopped");
}


/* B4: 校验会话 token（供 2939 WS 通道认证） */

int connection_server_is_running(void) {
    LOG_DEBUG_T("Connection", "IsRunning", "result", "returning %d", g_server_running);
    return g_server_running;
}

int connection_get_active_sessions(void) {
    int count = 0;
    pthread_mutex_lock(&g_session_lock);
    connection_session_t *sess = g_sessions;
    while (sess) {
        if (sess->is_active) count++;
        sess = sess->next;
    }
    pthread_mutex_unlock(&g_session_lock);
    LOG_DEBUG_T("Connection", "ActiveSessions", "count", "count=%d", count);
    return count;
}

const connection_config_t* connection_get_config(void) {
    LOG_DEBUG_T("Connection", "GetConfig", "config", "returning config", "");
    return &g_config;
}

int connection_load_config(const char *path) {
    LOG_INFO_T("Connection", "LoadConfig", "Enter", "path='%s'", path ? path : "(null)");
    if (!path) {
        const char *root = lingos_data_root();
        static char full_path[512];
        safe_snprintf(full_path, sizeof(full_path), "%s/system/config/network.conf", root);
        path = full_path;
        LOG_DEBUG_T("Connection", "LoadConfig", "DefaultPath", "using default: %s", path);
    }

    FILE *fp = fopen(path, "r");
    if (!fp) {
        LOG_DEBUG_T("Connection", "LoadConfig", "NotFound", "config file not found, using defaults");
        return 0;
    }

    char line[256];
    while (fgets(line, sizeof(line), fp)) {
        if (line[0] == '#' || line[0] == '\n' || line[0] == '\r') continue;
        char key[64], val[64];
        if (sscanf(line, "%63[^=]=%63s", key, val) == 2) {
            LOG_DEBUG_T("Connection", "LoadConfig", "Item", "key='%s', val='%s'", key, val);
            if (strcmp(key, "app_port") == 0) g_config.primary_port = atoi(val);
            else if (strcmp(key, "app_backup_port") == 0) g_config.backup_port = atoi(val);
            else if (strcmp(key, "hardware_port") == 0) g_config.hardware_port = atoi(val);
            else if (strcmp(key, "max_connections") == 0) g_config.max_clients = atoi(val);
            else if (strcmp(key, "connection_timeout") == 0) g_config.auth_timeout = atoi(val);
            else if (strcmp(key, "heartbeat_interval") == 0) g_config.heartbeat_interval = atoi(val);
            else if (strcmp(key, "code_expire_seconds") == 0) g_config.code_expire_seconds = atoi(val);
        }
    }
    fclose(fp);

    LOG_INFO_T("Connection", "LoadConfig", "OK", "port=%d backup=%d max=%d timeout=%d",
               g_config.primary_port, g_config.backup_port, g_config.max_clients, g_config.auth_timeout);
    return 0;
}

int connection_save_config(const char *path) {
    LOG_INFO_T("Connection", "SaveConfig", "Enter", "path='%s'", path ? path : "(null)");
    if (!path) {
        const char *root = lingos_data_root();
        static char full_path[512];
        safe_snprintf(full_path, sizeof(full_path), "%s/system/config/network.conf", root);
        path = full_path;
        LOG_DEBUG_T("Connection", "SaveConfig", "DefaultPath", "using default: %s", path);
    }

    char dir[512];
    const char *root = lingos_data_root();
    safe_snprintf(dir, sizeof(dir), "%s/system/config", root);
    if (access(dir, F_OK) != 0) {
        LOG_DEBUG_T("Connection", "SaveConfig", "Mkdir", "creating directory: %s", dir);
        if (mkdir(dir, 0755) != 0) {
            LOG_ERROR_T("Connection", "SaveConfig", "MkdirFail", "mkdir %s failed: %s", dir, strerror(errno));
            return -1;
        }
    }

    FILE *fp = fopen(path, "w");
    if (!fp) {
        LOG_ERROR_T("Connection", "SaveConfig", "OpenFail", "fopen %s failed: %s", path, strerror(errno));
        return -1;
    }

    fprintf(fp, "# LING OS Network Configuration\n");
    fprintf(fp, "# Auto-generated\n\n");
    fprintf(fp, "app_port = %d\n", g_config.primary_port);
    fprintf(fp, "app_backup_port = %d\n", g_config.backup_port);
    fprintf(fp, "hardware_port = %d\n", g_config.hardware_port);
    fprintf(fp, "max_connections = %d\n", g_config.max_clients);
    fprintf(fp, "connection_timeout = %d\n", g_config.auth_timeout);
    fprintf(fp, "heartbeat_interval = %d\n", g_config.heartbeat_interval);
    fprintf(fp, "code_expire_seconds = %d\n", g_config.code_expire_seconds);

    fclose(fp);
    LOG_INFO_T("Connection", "SaveConfig", "OK", "saved to %s", path);
    return 0;
}

/* ============================================================
 * 【先生设计】令牌系统扩展（v2）
 * - no_verify（token remove login <ip>——该 IP 免验证——受限模式）
 * - pending 重验证（token login again <code>——0x0003 匹配——新 token 签发）
 * - token 命令族（add/remove/add login/remove login——tokens.json 管理）
 * ============================================================ */
#define NO_VERIFY_FILE "/LINGOS/state/no_verify.json"
#define PENDING_REVAL_FILE "/LINGOS/state/pending_revalidate.json"

/* ---------- no_verify（免验证——IP 维度） ---------- */
static int no_verify_check_locked(const char *ip) {
    if (!ip || !ip[0]) return 0;
    char buf[8192];
    FILE *f = fopen(NO_VERIFY_FILE, "r");
    if (!f) return 0;
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    buf[n] = '\0';
    fclose(f);
    /* 简易解析：查 ip + until */
    char *pos = strstr(buf, ip);
    if (!pos) return 0;
    char *u = strstr(pos, "until");
    long until = 0;
    if (u) sscanf(u, "until\":%ld", &until);
    if (until == 0 || until > time(NULL)) return 1;  /* no(永久) 或未到期 */
    return 0;
}

/** 【先生设计】检查 IP 是否免验证（移除登录验证——不再验证令牌——受限模式） */
int connection_no_verify_check(const char *ip) {
    if (!ip || !ip[0]) return 0;
    return no_verify_check_locked(ip);
}

/** 【先生设计】设置/移除 IP 免验证（token remove login <ip> [time]） */
int connection_no_verify_set(const char *ip, const char *time_str, int permanent) {
    if (!ip || !ip[0]) return -1;
    long until = 0;
    if (!permanent && time_str && time_str[0]) {
        /* 解析时间：y(年)/m(月)/d(天)/s(秒)——默认 1d */
        char unit = 'd';
        long val = 1;
        if (sscanf(time_str, "%ld%c", &val, &unit) < 1) { val = 1; unit = 'd'; }
        time_t now = time(NULL);
        if (unit == 'y') until = now + val * 365L * 86400L;
        else if (unit == 'm') until = now + val * 30L * 86400L;
        else if (unit == 'd') until = now + val * 86400L;
        else if (unit == 's') until = now + val;
        else until = now + 86400L;  /* 默认 1d */
    } else if (permanent) {
        until = 0;  /* no——永久（除非手动移除） */
    } else {
        until = time(NULL) + 86400L;  /* 默认 1d */
    }
    /* 写 no_verify.json（保留已有——追加/更新该 ip） */
    char buf[8192];
    buf[0] = '\0';
    FILE *rf = fopen(NO_VERIFY_FILE, "r");
    if (rf) {
        size_t n = fread(buf, 1, sizeof(buf) - 1, rf);
        buf[n] = '\0';
        fclose(rf);
    }
    /* 移除该 ip 旧条目（按 ip 子串定位条目重写——简易：过滤含 ip 的条目） */
    /* 组装新条目 */
    char entry[256];
    safe_snprintf(entry, sizeof(entry), "{\"ip\":\"%s\",\"until\":%ld},",
                  ip, (long)until);
    char final_buf[9000];
    if (buf[0]) {
        size_t fl = strlen(buf);
        while (fl > 0 && (buf[fl-1] == ']' || buf[fl-1] == '\n' || buf[fl-1] == ' ')) fl--;
        safe_snprintf(final_buf, sizeof(final_buf), "%.*s,%s]", (int)fl, buf, entry + 1);
    } else {
        safe_snprintf(final_buf, sizeof(final_buf), "[%s]", entry);
    }
    FILE *wf = fopen(NO_VERIFY_FILE, "w");
    if (!wf) return -1;
    fwrite(final_buf, 1, strlen(final_buf), wf);
    fclose(wf);
    LOG_WARN_T("Token", "NoVerify", "Set", "ip=%s until=%ld (permanent=%d)", ip, (long)until, permanent);
    return 0;
}

/** 【先生设计】移除 IP 免验证（Reverify——重新验证） */
int connection_no_verify_remove(const char *ip) {
    if (!ip || !ip[0]) return -1;
    char buf[8192];
    buf[0] = '\0';
    FILE *rf = fopen(NO_VERIFY_FILE, "r");
    if (!rf) return 0;
    size_t n = fread(buf, 1, sizeof(buf) - 1, rf);
    buf[n] = '\0';
    fclose(rf);
    if (!buf[0]) return 0;
    /* 重建：过滤含该 ip 的条目 */
    char out[8192];
    out[0] = '\0';
    size_t o = 0;
    /* 简易：按逗号分隔条目——跳过含 ip 的 */
    char *save = NULL;
    char *tok = strtok_r(buf + 1, ",", &save);  /* 跳过 [ */
    char first[9000];
    first[0] = '\0';
    while (tok) {
        char *close = strchr(tok, '}');
        if (close) {
            char item[256];
            size_t il = close - tok + 1;
            if (il < sizeof(item)) {
                memcpy(item, tok, il);
                item[il] = '\0';
                if (!strstr(item, ip)) {
                    /* 保留 */
                    if (first[0]) {
                        size_t l = strlen(out);
                        if (l + 1 < sizeof(out)) { out[l] = ','; out[l+1] = '\0'; }
                    }
                    size_t l = strlen(out);
                    if (l + il < sizeof(out)) memcpy(out + l, item, il);
                    first[0] = 1;
                }
            }
        }
        tok = strtok_r(NULL, ",", &save);
    }
    char final_buf[9000];
    if (out[0]) {
        safe_snprintf(final_buf, sizeof(final_buf), "[%s]", out);
    } else {
        safe_snprintf(final_buf, sizeof(final_buf), "[]");
    }
    FILE *wf = fopen(NO_VERIFY_FILE, "w");
    if (!wf) return -1;
    fwrite(final_buf, 1, strlen(final_buf), wf);
    fclose(wf);
    LOG_WARN_T("Token", "NoVerify", "Remove", "ip=%s (Reverify)", ip);
    return 0;
}

/* ---------- pending 重验证（token login again） ---------- */
static char g_pending_code[64] = {0};
static char g_pending_ip[64] = {0};
static long g_pending_expire = 0;
static pthread_mutex_t g_pending_lock = PTHREAD_MUTEX_INITIALIZER;

/** 【先生设计】设置 pending 验证码（token login again <code>——主机端） */
void connection_pending_set(const char *code, const char *ip, int ttl_seconds) {
    pthread_mutex_lock(&g_pending_lock);
    safe_strncpy(g_pending_code, code ? code : "", sizeof(g_pending_code));
    safe_strncpy(g_pending_ip, ip ? ip : "", sizeof(g_pending_ip));
    g_pending_expire = time(NULL) + (ttl_seconds > 0 ? ttl_seconds : 60);
    pthread_mutex_unlock(&g_pending_lock);
    LOG_WARN_T("Token", "Pending", "Set", "code=%s ip=%s expire=%ld", code ? code : "", ip ? ip : "", (long)g_pending_expire);
}

/** 【先生设计】匹配 pending 验证码（App 发 0x0003——payload=验证码） */
int connection_pending_match(const char *code, const char *ip) {
    pthread_mutex_lock(&g_pending_lock);
    int ok = 0;
    if (g_pending_code[0] && code && code[0] &&
        strcmp(g_pending_code, code) == 0 &&
        time(NULL) < g_pending_expire) {
        ok = 1;
    }
    if (ok) {
        g_pending_code[0] = '\0';
        g_pending_expire = 0;
    }
    pthread_mutex_unlock(&g_pending_lock);
    return ok;
}

/** 【先生设计】token add（添加令牌——指定或自动生成） */
int connection_token_add(const char *token_arg, const char *ip, const char *uid, long ttl_seconds) {
    char token[64];
    if (token_arg && token_arg[0]) {
        safe_strncpy(token, token_arg, sizeof(token));
    } else {
        /* 自动生成 32 hex */
        FILE *ur = fopen("/dev/urandom", "r");
        unsigned char rnd[16];
        if (!ur || fread(rnd, 1, 16, ur) != 16) {
            if (ur) fclose(ur);
            return -1;
        }
        if (ur) fclose(ur);
        for (int i = 0; i < 16; i++) snprintf(token + i * 2, 3, "%02x", rnd[i]);
    }
    connection_store_token(token, ttl_seconds > 0 ? ttl_seconds : 2592000);
    LOG_WARN_T("Token", "Add", "OK", "token=%s ip=%s uid=%s ttl=%ld", token, ip ? ip : "-", uid ? uid : "-", (long)ttl_seconds);
    return 0;
}

/** 【先生设计】token remove（移除令牌） */
int connection_token_remove(const char *token) {
    if (!token || !token[0]) return -1;
    connection_revoke_token_store(token);
    LOG_WARN_T("Token", "Remove", "OK", "token=%s", token);
    return 0;
}
