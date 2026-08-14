/**
 * @file    src/api/websocket_server.c
 * @brief   WebSocket 服务器（实时推送 + 多客户端管理）
 * @version LN-B-5.1.2.6-rc
 * @par     核心协议：C1, C-C, AI-CTL
 * @changes 多客户端管理；主题订阅；消息广播；心跳保活
 */

#include "websocket_server.h"
#include "connection_handler.h"
#include "../ai/ai_server_protocol.h"
#include "../common/safe_string.h"
#include "../common/data_path.h"
#include "../common/lang.h"
#include "../lib/log_extra.h"
#include "../lib/cJSON/cJSON.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <stdint.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <errno.h>
#include <time.h>
#include <poll.h>
#include <sys/select.h>
/* websocket_server.c 补 sys/select.h（fd_set/select 需显式——musl） */

#define MAX_CLIENTS 32
#define WS_GUID "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"
#define BUFFER_SIZE 4096
#define HEARTBEAT_INTERVAL 30

/* ============================================================
 * WebSocket 客户端结构
 * ============================================================ */
typedef struct ws_client {
    int fd;
    int authenticated;      /* B4: token 认证通过 */
    char token[64];         /* B4: 会话 token */
    char ip[INET_ADDRSTRLEN]; /* 【先生设计】客户端 IP（no_verify 免验证检查） */
    int restricted;         /* 【先生设计】受限模式（免验证连接——危险操作拒绝） */
    char id[32];
    int active;
    char topics[8][64];
    int topic_count;
    time_t connected_at;
    time_t last_heartbeat;
    volatile int chat_active;    /* 【修复A2】chat 转发线程运行中 */
    volatile int chat_interrupt; /* 【修复A2】收到 interrupt 帧——请求中断当前 chat */
    struct ws_client *next;
    pthread_mutex_t lock;
} ws_client_t;

/* ============================================================
 * 全局状态
 * ============================================================ */
static ws_client_t *g_clients = NULL;
static int g_ws_running = 0;
static pthread_t g_ws_thread;
static pthread_t g_heartbeat_thread;
static pthread_mutex_t g_ws_lock = PTHREAD_MUTEX_INITIALIZER;
static volatile int g_heartbeat_stop = 0;

/* ============================================================
 * FTF[生成 WebSocket 握手响应]
 * ============================================================ */
/* ============================================================
 * 【修复】WebSocket Sec-WebSocket-Accept 正确计算（RFC 6455）
 * accept = base64( SHA1( key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11" ) )
 * 原实现直接回显 key——Dart 客户端严格校验 → 握手失败（Connection closed）
 * ============================================================ */

static void ws_sha1(const unsigned char *input, size_t len, unsigned char output[20]) {
    unsigned int h0 = 0x67452301, h1 = 0xEFCDAB89, h2 = 0x98BADCFE, h3 = 0x10325476, h4 = 0xC3D2E1F0;
    size_t msg_len = len;
    size_t padded = ((msg_len + 8) / 64 + 1) * 64;
    unsigned char *msg = calloc(1, padded);
    if (!msg) return;
    memcpy(msg, input, msg_len);
    msg[msg_len] = 0x80;
    unsigned long long bits = (unsigned long long)msg_len * 8;
    for (int i = 0; i < 8; i++) msg[padded - 1 - i] = (bits >> (i * 8)) & 0xFF;
    for (size_t off = 0; off < padded; off += 64) {
        unsigned int w[80];
        for (int i = 0; i < 16; i++)
            w[i] = (msg[off + i*4] << 24) | (msg[off + i*4+1] << 16) | (msg[off + i*4+2] << 8) | msg[off + i*4+3];
        for (int i = 16; i < 80; i++) {
            unsigned int v = w[i-3] ^ w[i-8] ^ w[i-14] ^ w[i-16];
            w[i] = (v << 1) | (v >> 31);
        }
        unsigned int a = h0, b = h1, c = h2, d = h3, e = h4;
        for (int i = 0; i < 80; i++) {
            unsigned int f, k;
            if (i < 20) { f = (b & c) | ((~b) & d); k = 0x5A827999; }
            else if (i < 40) { f = b ^ c ^ d; k = 0x6ED9EBA1; }
            else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDC; }
            else { f = b ^ c ^ d; k = 0xCA62C1D6; }
            unsigned int temp = ((a << 5) | (a >> 27)) + f + e + k + w[i];
            e = d; d = c; c = (b << 30) | (b >> 2); b = a; a = temp;
        }
        h0 += a; h1 += b; h2 += c; h3 += d; h4 += e;
    }
    free(msg);
    output[0] = h0 >> 24; output[1] = h0 >> 16; output[2] = h0 >> 8; output[3] = h0;
    output[4] = h1 >> 24; output[5] = h1 >> 16; output[6] = h1 >> 8; output[7] = h1;
    output[8] = h2 >> 24; output[9] = h2 >> 16; output[10] = h2 >> 8; output[11] = h2;
    output[12] = h3 >> 24; output[13] = h3 >> 16; output[14] = h3 >> 8; output[15] = h3;
    output[16] = h4 >> 24; output[17] = h4 >> 16; output[18] = h4 >> 8; output[19] = h4;
}

static void ws_base64(const unsigned char *in, size_t len, char *out, size_t out_size) {
    static const char tbl[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t i, o = 0;
    for (i = 0; i + 2 < len || i < len; i += 3) {
        unsigned int n = 0;
        int remain = (int)(len - i);
        if (remain > 0) n |= in[i] << 16;
        if (remain > 1) n |= in[i+1] << 8;
        if (remain > 2) n |= in[i+2];
        if (o + 4 < out_size) {
            out[o++] = tbl[(n >> 18) & 63];
            out[o++] = tbl[(n >> 12) & 63];
            out[o++] = remain > 1 ? tbl[(n >> 6) & 63] : '=';
            out[o++] = remain > 2 ? tbl[n & 63] : '=';
        }
        if (remain <= 2) break;
    }
    if (o < out_size) out[o] = '\0';
}

static void ws_accept(const char *key, char *out, size_t out_size) {
    const char *guid = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
    size_t klen = strlen(key);
    unsigned char buf[256];
    memcpy(buf, key, klen);
    memcpy(buf + klen, guid, strlen(guid));
    unsigned char digest[20];
    ws_sha1(buf, klen + strlen(guid), digest);
    ws_base64(digest, 20, out, out_size);
}

/* ============================================================
 * 【修复】SHA-1 + Base64（RFC 6455 Sec-WebSocket-Accept 计算）
 * accept = base64( SHA1(key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11") )
 * 原实现直接回显 key——Dart/web_socket_channel 校验失败→Connection closed
 * ============================================================ */


static void ws_handshake(int fd, const char *key) {
    char response[512];
    char key_b64[40];
    /* RFC 6455: SHA1(key + GUID) base64——用已有 ws_accept（ws_sha1/ws_base64 正确实现） */
    ws_accept(key, key_b64, sizeof(key_b64));
    safe_snprintf(response, sizeof(response),
        "HTTP/1.1 101 Switching Protocols\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Accept: %s\r\n\r\n",
        key_b64);
    write(fd, response, strlen(response));
    LOG_DEBUG_T("WebSocket", "Handshake", "OK", "handshake completed for fd=%d", fd);
}

/* ============================================================
 * FTF[读取 WebSocket 帧头（简化实现）]
 * ============================================================ */
static int read_ws_frame(int fd, char *payload, size_t payload_size) {
    unsigned char header[2];
    ssize_t n = read(fd, header, 2);
    if (n != 2) return -1;

    int fin = (header[0] & 0x80) >> 7;
    int opcode = header[0] & 0x0F;
    int mask = (header[1] & 0x80) >> 7;
    int payload_len = header[1] & 0x7F;

    (void)fin;
    (void)opcode;

    if (payload_len >= 126) {
        unsigned char ext[2];
        if (read(fd, ext, 2) != 2) return -1;
        payload_len = (ext[0] << 8) | ext[1];
    }

    unsigned char mask_key[4];
    if (mask) {
        if (read(fd, mask_key, 4) != 4) return -1;
    }

    if (payload_len > (int)payload_size - 1) {
        payload_len = payload_size - 1;
    }

    if (payload_len > 0) {
        ssize_t read_len = read(fd, payload, payload_len);
        if (read_len <= 0) return -1;
        payload_len = read_len;
    }
    payload[payload_len] = '\0';

    if (mask) {
        for (int i = 0; i < payload_len; i++) {
            payload[i] ^= mask_key[i % 4];
        }
    }

    return payload_len;
}

/* ============================================================
 * FTF[发送 WebSocket 帧（简化实现）]
 * ============================================================ */
static int send_ws_frame(int fd, const char *data) {
    size_t len = strlen(data);
    unsigned char header[6];
    header[0] = 0x81;
    if (len <= 125) {
        header[1] = len;
        if (write(fd, header, 2) != 2) return -1;
        if (write(fd, data, len) != (ssize_t)len) return -1;
    } else if (len <= 65535) {
        header[1] = 126;
        header[2] = (len >> 8) & 0xFF;
        header[3] = len & 0xFF;
        if (write(fd, header, 4) != 4) return -1;
        if (write(fd, data, len) != (ssize_t)len) return -1;
    } else {
        return -1;
    }
    return 0;
}

/* ============================================================
 * FTF[查找客户端]
 * ============================================================ */
static ws_client_t* find_client(int fd) {
    pthread_mutex_lock(&g_ws_lock);
    ws_client_t *c = g_clients;
    while (c) {
        if (c->fd == fd && c->active) {
            pthread_mutex_unlock(&g_ws_lock);
            return c;
        }
        c = c->next;
    }
    pthread_mutex_unlock(&g_ws_lock);
    return NULL;
}

/* ============================================================
 * FTF[添加客户端]
 * ============================================================ */
static ws_client_t* add_client(int fd) {
    ws_client_t *client = calloc(1, sizeof(ws_client_t));
    if (!client) return NULL;

    client->fd = fd;
    client->active = 1;
    client->connected_at = time(NULL);
    client->last_heartbeat = time(NULL);
    pthread_mutex_init(&client->lock, NULL);
    safe_snprintf(client->id, sizeof(client->id), "ws_%d_%ld", fd, time(NULL));

    pthread_mutex_lock(&g_ws_lock);
    client->next = g_clients;
    g_clients = client;
    pthread_mutex_unlock(&g_ws_lock);

    LOG_INFO_T("WebSocket", "AddClient", "OK", "client %s added", client->id);
    return client;
}

/* ============================================================
 * FTF[移除客户端]
 * ============================================================ */
static void remove_client(int fd) {
    pthread_mutex_lock(&g_ws_lock);
    ws_client_t *prev = NULL;
    ws_client_t *c = g_clients;
    while (c) {
        if (c->fd == fd) {
            if (prev) prev->next = c->next;
            else g_clients = c->next;
            c->active = 0;
            close(c->fd);
            pthread_mutex_destroy(&c->lock);
            free(c);
            LOG_INFO_T("WebSocket", "RemoveClient", "OK", "removed client fd=%d", fd);
            pthread_mutex_unlock(&g_ws_lock);
            return;
        }
        prev = c;
        c = c->next;
    }
    pthread_mutex_unlock(&g_ws_lock);
}

/* ============================================================
 * 【修复A2】chat 转发独立线程（主循环不被阻塞——interrupt 帧可达）
 * ============================================================ */
typedef struct ws_chat_arg {
    ws_client_t *client;
    char *prompt;
    char *model;
    char *image;
} ws_chat_arg_t;

static void* ws_chat_thread(void *argp) {
    ws_chat_arg_t *a = (ws_chat_arg_t *)argp;
    ws_client_t *client = a ? a->client : NULL;
    const char *prompt = a ? a->prompt : NULL;
    const char *model = a ? a->model : NULL;
    const char *image = a ? a->image : NULL;

    if (!client || !prompt) goto done;
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) goto done;
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    safe_strncpy(addr.sun_path, AI_SOCKET_PATH, sizeof(addr.sun_path));
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        close(fd);
        pthread_mutex_lock(&client->lock);
        if (client->active) send_ws_frame(client->fd, "{\"type\":\"chat_error\",\"message\":\"AI server unavailable\"}");
        pthread_mutex_unlock(&client->lock);
        goto done;
    }

    cJSON *root = cJSON_CreateObject();
    if (!root) { close(fd); goto done; }
    cJSON_AddStringToObject(root, "cmd", "nook_ask_stream");
    cJSON_AddStringToObject(root, "prompt", prompt);
    if (model && model[0] != '\0') {
        cJSON_AddStringToObject(root, "model", model);   /* B10: 模型透传 */
    }
    if (image && image[0] != '\0') {
        cJSON_AddStringToObject(root, "image", image);   /* R6: 多模态透传 */
    }
    cJSON_AddNumberToObject(root, "timeout", 60);
    cJSON_AddBoolToObject(root, "gui_mode", 1); /* B5: GUI 模式提示词 */
    char *msg = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!msg) { close(fd); goto done; }
    write(fd, msg, strlen(msg));
    write(fd, "\n", 1);
    free(msg);

    /* 流式读取——poll 100ms 超时轮询 interrupt 标志 */
    /* 【0.2.1 #3 修复】行缓冲 4096 → 64KB：超长事件行（工具结果/思考流）截断会损坏 JSON
       导致 App 解析失败丢事件 → 对话"中断"假象 */
    char line_buf[65536];
    size_t line_pos = 0;
    int interrupted = 0;
    while (1) {
        if (client->chat_interrupt) { interrupted = 1; break; }
        struct pollfd pfd;
        pfd.fd = fd;
        pfd.events = POLLIN;
        int pr = poll(&pfd, 1, 100);
        if (pr < 0) break;
        if (pr == 0) continue;  /* 超时——回循环检查 interrupt */
        ssize_t n = read(fd, line_buf + line_pos, 1);
        if (n <= 0) break;
        if (line_buf[line_pos] == '\n') {
            line_buf[line_pos] = '\0';
            line_pos = 0;
            if (line_buf[0] == '\0') continue;
            char chunk_send[66000];
            safe_snprintf(chunk_send, sizeof(chunk_send),
                          "{\"type\":\"chat_event\",\"data\":%s}", line_buf);
            pthread_mutex_lock(&client->lock);
            if (client->active) send_ws_frame(client->fd, chunk_send);
            pthread_mutex_unlock(&client->lock);
            if (strstr(line_buf, "\"type\":\"done\"") || strstr(line_buf, "\"type\":\"error\"")) break;
        } else {
            line_pos++;
            if (line_pos >= sizeof(line_buf) - 1) {
                /* 行超长（>64KB）：丢弃该行剩余部分直到换行，防止损坏 JSON 上传 */
                line_buf[0] = '\0';
                line_pos = 0;
                while (1) {
                    ssize_t c = read(fd, line_buf, 1);
                    if (c <= 0) break;
                    if (line_buf[0] == '\n') break;
                }
            }
        }
    }
    close(fd);

    pthread_mutex_lock(&client->lock);
    if (client->active) {
        if (interrupted) {
            send_ws_frame(client->fd, "{\"type\":\"chat_interrupted\"}");
        } else {
            send_ws_frame(client->fd, "{\"type\":\"chat_done\"}");
        }
    }
    client->chat_active = 0;
    client->chat_interrupt = 0;
    pthread_mutex_unlock(&client->lock);

done:
    if (a) {
        if (a->client) {
            pthread_mutex_lock(&a->client->lock);
            a->client->chat_active = 0;
            a->client->chat_interrupt = 0;
            pthread_mutex_unlock(&a->client->lock);
        }
        free(a->prompt);
        free(a->model);
        free(a->image);
        free(a);
    }
    return NULL;
}

/* ============================================================
 * 【先生决策】App 命令转发：command → ai.sock（Python）→ 响应回推
 * ============================================================ */
static void ws_forward_command(ws_client_t *client, const char *cmd_json) {
    if (!client || !cmd_json) return;
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return;
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    safe_strncpy(addr.sun_path, AI_SOCKET_PATH, sizeof(addr.sun_path));
    struct timeval tv;
    tv.tv_sec = 15;
    tv.tv_usec = 0;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) == 0) {
        char req[8192];
        safe_snprintf(req, sizeof(req), "%s\n", cmd_json);
        ssize_t n = write(fd, req, strlen(req));
        if (n > 0) {
            char buf[16384];
            ssize_t r = read(fd, buf, sizeof(buf) - 1);
            if (r > 0) {
                buf[r] = '\0';
                /* 剥离可能的尾部换行 */
                if (r > 0 && buf[r - 1] == '\n') buf[r - 1] = '\0';
                char out[16500];
                safe_snprintf(out, sizeof(out), "{\"type\":\"command_response\",\"data\":%s}", buf);
                send_ws_frame(client->fd, out);
            } else {
                send_ws_frame(client->fd, "{\"type\":\"command_response\",\"data\":{\"status\":\"error\",\"msg\":\"no response\"}}");
            }
        }
    } else {
        send_ws_frame(client->fd, "{\"type\":\"command_response\",\"data\":{\"status\":\"error\",\"msg\":\"ai server unavailable\"}}");
    }
    close(fd);
}

/* ============================================================
 * 【协议v3】App 审批结果转发：auth_resp → auth.sock（authorization_service）
 * ============================================================ */
static void ws_forward_auth_resp(ws_client_t *client, const char *req_id, int approved) {
    if (!client || !req_id) return;
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return;
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    safe_strncpy(addr.sun_path, "/LINGOS/run/auth.sock", sizeof(addr.sun_path));
    struct timeval tv;
    tv.tv_sec = 2;
    tv.tv_usec = 0;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) == 0) {
        char req[512];
        safe_snprintf(req, sizeof(req),
                      "{\"cmd\":\"%s\",\"request_id\":\"%s\",\"reason\":\"App approval\"}\n",
                      approved ? "approve" : "reject", req_id);
        ssize_t n = write(fd, req, strlen(req));
        if (n > 0) {
            char buf[256] = {0};
            ssize_t r = read(fd, buf, sizeof(buf) - 1);
            LOG_DEBUG_T("WebSocket", "AuthResp", "Forwarded",
                        "req_id=%s approved=%d resp=%s", req_id, approved, r > 0 ? buf : "(no resp)");
        }
    } else {
        LOG_WARN_T("WebSocket", "AuthResp", "ConnectFail",
                   "cannot connect auth.sock: %s", strerror(errno));
    }
    close(fd);
    send_ws_frame(client->fd, "{\"type\":\"auth_resp_ok\"}");
}

static void process_client_message(ws_client_t *client, const char *msg) {
    if (!client || !msg) return;

    /* 简单解析：{"type":"subscribe","topic":"/system/status"} */
    char *p = strstr(msg, "\"type\"");
    if (!p) return;

    char type[32] = {0};
    char *p1 = strchr(p, ':');
    if (p1) {
        p1++;
        while (*p1 == ' ' || *p1 == '\t') p1++;
        if (*p1 == '"') {
            p1++;
            char *end = strchr(p1, '"');
            if (end) {
                int len = end - p1;
                if (len < (int)sizeof(type) - 1) {
                    strncpy(type, p1, len);
                    type[len] = '\0';
                }
            }
        }
    }

    /* 【先生设计】受限模式（免验证连接——危险操作拒绝） */
    if (client->restricted) {
        const char *dangerous[] = {"rm -rf", "rm /", "reboot", "shutdown", "format", "mkfs", "dd if", "> /dev/sd", "kill -9 1", "rm /LINGOS", "systemctl stop", "poweroff"};
        for (size_t i = 0; i < sizeof(dangerous)/sizeof(dangerous[0]); i++) {
            if (msg && strstr(msg, dangerous[i])) {
                send_ws_frame(client->fd, "{\"type\":\"error\",\"message\":\"受限模式：危险操作不允许（免验证连接）\"}");
                return;
            }
        }
    }
    /* B4: token 认证 + chat 数据流（App 经 2939 对话） */
    /* 【诊断】WS 认证日志（WARN——先生日志可见） */
    if (strcmp(type, "auth") == 0) {
        LOG_WARN_T("WebSocket", "Auth", "Recv", "WS auth attempt from %s", client->id);
        char token[64] = {0};
        char *tp = strstr(msg, "\"token\"");
        if (tp) {
            char *p2 = strchr(tp, ':');
            if (p2) {
                p2++;
                while (*p2 == ' ' || *p2 == '\t') p2++;
                if (*p2 == '"') {
                    p2++;
                    char *end = strchr(p2, '"');
                    if (end) {
                        int len = end - p2;
                        if (len < (int)sizeof(token) - 1) { memcpy(token, p2, len); token[len] = '\0'; }
                    }
                }
            }
        }
        int no_verify_ok = 0;
        if (client->ip[0] && connection_no_verify_check(client->ip)) {
            /* 【先生设计】该 IP 免验证（token remove login）——直接通过（受限模式） */
            no_verify_ok = 1;
            client->restricted = 1;
            LOG_WARN_T("WebSocket", "Auth", "NoVerify", "ip=%s 免验证通过（受限模式）", client->ip);
        }
        if (no_verify_ok || connection_verify_token(token)) {
            client->authenticated = 1;
            safe_strncpy(client->token, token, sizeof(client->token));
            LOG_WARN_T("WebSocket", "Auth", "TokenOK", "token valid, client=%s", client->id);
            /* 【先生决策】设备绑定：解析 device_id → 绑定 + 校验 */
            const char *dev = NULL;
            char *d1 = strstr(msg, "\"device_id\"");
            if (d1) {
                char *c = strchr(d1, ':');
                if (c) {
                    c++;
                    while (*c == ' ' || *c == '\t') c++;
                    if (*c == '"') {
                        c++;
                        char *end = strrchr(c, '"');
                        if (end) {
                            static char devbuf[64];
                            int dl = end - c;
                            if (dl > 63) dl = 63;
                            memcpy(devbuf, c, dl);
                            devbuf[dl] = '\0';
                            dev = devbuf;
                        }
                    }
                }
            }
            if (dev) {
                if (!connection_verify_token_device(token, dev)) {
                    LOG_WARN_T("WebSocket", "Auth", "DeviceFail", "device mismatch, client=%s", client->id);
                    send_ws_frame(client->fd, "{\"type\":\"auth_error\",\"message\":\"token bound to another device\"}");
                    return;
                }
                connection_bind_device(token, dev);
            }
            send_ws_frame(client->fd, "{\"type\":\"auth_ok\"}");
        } else {
            LOG_WARN_T("WebSocket", "Auth", "TokenFail", "invalid token, client=%s", client->id);
            send_ws_frame(client->fd, "{\"type\":\"auth_error\",\"message\":\"invalid token\"}");
        }
        return;
    }
    if (strcmp(type, "chat") == 0 && client->authenticated) {
        char prompt[2048] = {0};
        char *pp = strstr(msg, "\"prompt\"");
        if (pp) {
            char *p2 = strchr(pp, ':');
            if (p2) {
                p2++;
                while (*p2 == ' ' || *p2 == '\t') p2++;
                if (*p2 == '"') {
                    p2++;
                    char *end = strrchr(p2, '"');
                    if (end) {
                        int len = end - p2;
                        if (len < (int)sizeof(prompt) - 1) { memcpy(prompt, p2, len); prompt[len] = '\0'; }
                    }
                }
            }
        }
        if (prompt[0] != '\0') {
            char model[64] = {0};
            char *mp = strstr(msg, "\"model\"");
            if (mp) {
                char *p2 = strchr(mp, ':');
                if (p2) {
                    p2++;
                    while (*p2 == ' ' || *p2 == '\t') p2++;
                    if (*p2 == '"') {
                        p2++;
                        char *end = strchr(p2, '"');
                        if (end) {
                            int len = end - p2;
                            if (len < (int)sizeof(model) - 1) { memcpy(model, p2, len); model[len] = '\0'; }
                        }
                    }
                }
            }
            /* 【B修复】image 动态分配（防栈溢出） */
            char *image_buf = (char *)calloc(2 * 1024 * 1024, 1);
            if (!image_buf) {
                pthread_mutex_lock(&client->lock);
                if (client->chat_active) {
                    pthread_mutex_unlock(&client->lock);
                    send_ws_frame(client->fd, "{\"type\":\"chat_error\",\"message\":\"busy\"}");
                    return;
                }
                client->chat_active = 1;
                client->chat_interrupt = 0;
                pthread_mutex_unlock(&client->lock);
                send_ws_frame(client->fd, "{\"type\":\"chat_error\",\"message\":\"AI server unavailable\"}");
                return;
            }
            char *im = strstr(msg, "\"image\"");
            if (im) {
                char *p2 = strchr(im, ':');
                if (p2) {
                    p2++;
                    while (*p2 == ' ' || *p2 == '\t') p2++;
                    if (*p2 == '"') {
                        p2++;
                        char *end = strchr(p2, '"');
                        if (end) {
                            int len = end - p2;
                            if (len > 0 && len < (int)sizeof(image_buf) - 1) {
                                memcpy(image_buf, p2, len);
                                image_buf[len] = '\0';
                            }
                        }
                    }
                }
            }
            /* 【修复A2】chat 转发独立线程——主循环继续收 interrupt 帧 */
            pthread_mutex_lock(&client->lock);
            if (client->chat_active) {
                pthread_mutex_unlock(&client->lock);
                free(image_buf);
                send_ws_frame(client->fd, "{\"type\":\"chat_error\",\"message\":\"busy\"}");
                return;
            }
            client->chat_active = 1;
            client->chat_interrupt = 0;
            pthread_mutex_unlock(&client->lock);

            ws_chat_arg_t *a = calloc(1, sizeof(ws_chat_arg_t));
            if (!a) {
                pthread_mutex_lock(&client->lock);
                client->chat_active = 0;
                pthread_mutex_unlock(&client->lock);
                free(image_buf);
                return;
            }
            a->client = client;
            a->prompt = strdup(prompt);
            a->model = (model[0] != '\0') ? strdup(model) : strdup("");
            a->image = (image_buf[0] != '\0') ? strdup(image_buf) : strdup("");
            free(image_buf);
            if (!a->prompt || !a->model || !a->image) {
                free(a->prompt); free(a->model); free(a->image); free(a);
                pthread_mutex_lock(&client->lock);
                client->chat_active = 0;
                pthread_mutex_unlock(&client->lock);
                return;
            }
            pthread_t chat_tid;
            if (pthread_create(&chat_tid, NULL, ws_chat_thread, a) != 0) {
                free(a->prompt); free(a->model); free(a->image); free(a);
                pthread_mutex_lock(&client->lock);
                client->chat_active = 0;
                pthread_mutex_unlock(&client->lock);
                send_ws_frame(client->fd, "{\"type\":\"chat_error\",\"message\":\"thread fail\"}");
                return;
            }
            pthread_detach(chat_tid);
        }
        return;
    }

    /* 【修复A2】interrupt 帧：中断当前 chat 转发 */
    if (strcmp(type, "interrupt") == 0 && client->authenticated) {
        pthread_mutex_lock(&client->lock);
        client->chat_interrupt = 1;
        pthread_mutex_unlock(&client->lock);
        send_ws_frame(client->fd, "{\"type\":\"interrupt_ack\"}");
        return;
    }

    char topic[64] = {0};
    p = strstr(msg, "\"topic\"");
    if (p) {
        p1 = strchr(p, ':');
        if (p1) {
            p1++;
            while (*p1 == ' ' || *p1 == '\t') p1++;
            if (*p1 == '"') {
                p1++;
                char *end = strchr(p1, '"');
                if (end) {
                    int len = end - p1;
                    if (len < (int)sizeof(topic) - 1) {
                        strncpy(topic, p1, len);
                        topic[len] = '\0';
                    }
                }
            }
        }
    }

    /* 【先生决策】注销：吊销 token */
    if (strcmp(type, "logout") == 0 && client->authenticated) {
        if (client->token[0]) {
            connection_revoke_token(client->token);
            connection_revoke_token_store(client->token);
        }
        send_ws_frame(client->fd, "{\"type\":\"logout_ok\"}");
        return;
    }

    /* 【先生决策】App 命令 → Python 直通 */
    if (strcmp(type, "command") == 0 && client->authenticated) {
        ws_forward_command(client, msg);
        return;
    }

    /* 【协议v3】App 审批结果 */
    if (strcmp(type, "auth_resp") == 0 && client->authenticated) {
        char req_id[128] = {0};
        int approved = 0;
        char *r1 = strstr(msg, "\"req_id\"");
        if (r1) {
            char *c = strchr(r1, ':');
            if (c) {
                c++;
                while (*c == ' ' || *c == '\t') c++;
                if (*c == '"') {
                    c++;
                    char *end = strrchr(c, '"');
                    if (end && end - c < (int)sizeof(req_id) - 1) {
                        memcpy(req_id, c, end - c);
                        req_id[end - c] = '\0';
                    }
                }
            }
        }
        char *a1 = strstr(msg, "\"approved\"");
        if (a1) {
            char *c = strchr(a1, ':');
            if (c) {
                c++;
                while (*c == ' ' || *c == '\t') c++;
                approved = (strncmp(c, "true", 4) == 0) || (*c == '1');
            }
        }
        if (req_id[0]) {
            ws_forward_auth_resp(client, req_id, approved);
        }
        return;
    }

    if (strcmp(type, "subscribe") == 0 && topic[0]) {
        pthread_mutex_lock(&client->lock);
        if (client->topic_count < 8) {
            safe_strncpy(client->topics[client->topic_count], topic, sizeof(client->topics[0]));
            client->topic_count++;
            LOG_DEBUG_T("WebSocket", "Subscribe", "OK", "client %s subscribed to %s", client->id, topic);
        }
        pthread_mutex_unlock(&client->lock);
    } else if (strcmp(type, "unsubscribe") == 0 && topic[0]) {
        pthread_mutex_lock(&client->lock);
        for (int i = 0; i < client->topic_count; i++) {
            if (strcmp(client->topics[i], topic) == 0) {
                for (int j = i; j < client->topic_count - 1; j++) {
                    safe_strncpy(client->topics[j], client->topics[j + 1], sizeof(client->topics[0]));
                }
                client->topic_count--;
                break;
            }
        }
        pthread_mutex_unlock(&client->lock);
        LOG_DEBUG_T("WebSocket", "Unsubscribe", "OK", "client %s unsubscribed from %s", client->id, topic);
    }
}

/* ============================================================
 * FTF[处理客户端连接]
 * ============================================================ */
typedef struct ws_thread_arg {
    int fd;
    char ip[INET_ADDRSTRLEN];
} ws_thread_arg_t;

static void handle_client(void *argp) {
    ws_thread_arg_t *arg = (ws_thread_arg_t *)argp;
    int client_fd = arg ? arg->fd : -1;
    const char *client_ip = arg ? arg->ip : NULL;
    if (arg) free(arg);
    if (client_fd < 0) return;
    char buf[BUFFER_SIZE];
    /* 【修复】分片问题：App(Android) 的 HTTP 请求可能 TCP 分片——循环读直到完整头(\r\n\r\n) */
    int total = 0;
    struct timeval tv;
    tv.tv_sec = 3;
    tv.tv_usec = 0;
    setsockopt(client_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    while (total < (int)sizeof(buf) - 1) {
        int n = read(client_fd, buf + total, sizeof(buf) - 1 - total);
        if (n <= 0) break;
        total += n;
        buf[total] = '\0';
        if (strstr(buf, "\r\n\r\n")) break;  /* 完整 HTTP 头 */
    }
    if (total <= 0) {
        close(client_fd);
        return;
    }

    /* 解析 WebSocket 握手 */
    char *key = strstr(buf, "Sec-WebSocket-Key:");
    if (!key) {
        close(client_fd);
        return;
    }

    key += 19;
    while (*key == ' ') key++;
    char *end = strstr(key, "\r\n");
    if (end) *end = '\0';

    /* 执行握手 */
    ws_handshake(client_fd, key);

    /* 添加客户端 */
    ws_client_t *client = add_client(client_fd);
    if (!client) {
        close(client_fd);
        return;
    }
    if (client_ip && client_ip[0]) {
        safe_strncpy(client->ip, client_ip, sizeof(client->ip));
    }

    /* 处理 WebSocket 帧 */
    while (client->active) {
        struct pollfd pfd;
        pfd.fd = client_fd;
        pfd.events = POLLIN;
        int ret = poll(&pfd, 1, 1000);
        if (ret < 0) break;
        if (ret == 0) {
            /* 心跳检测 */
            time_t now = time(NULL);
            if (now - client->last_heartbeat > HEARTBEAT_INTERVAL * 2) {
                LOG_WARN_T("WebSocket", "Heartbeat", "Timeout", "client %s heartbeat timeout", client->id);
                break;
            }
            continue;
        }

        char payload[BUFFER_SIZE];
        int len = read_ws_frame(client_fd, payload, sizeof(payload));
        if (len <= 0) break;

        client->last_heartbeat = time(NULL);

        if (len > 0) {
            process_client_message(client, payload);
        }
    }

    /* 【修复A2】等待 chat 转发线程退出（限时 2s）——避免释放后悬垂 */
    int wc = 0;
    while (client->chat_active && wc < 40) {
        usleep(50000);
        wc++;
    }

    remove_client(client_fd);
}

/* ============================================================
 * FTF[广播消息到订阅的客户端]
 * ============================================================ */
int websocket_broadcast(const char *topic, const char *message) {
    LOG_DEBUG_T("WebSocket", "Broadcast", "Enter", "topic='%s'", topic ? topic : "(null)");

    if (!topic || !message || !g_ws_running) return -1;

    /* 构造 JSON 消息 */
    char frame[BUFFER_SIZE];
    safe_snprintf(frame, sizeof(frame), "{\"topic\":\"%s\",\"data\":%s,\"timestamp\":%ld}",
                  topic, message, (long)time(NULL));

    int sent_count = 0;

    pthread_mutex_lock(&g_ws_lock);
    ws_client_t *c = g_clients;
    while (c) {
        if (c->active) {
            int subscribed = 0;
            pthread_mutex_lock(&c->lock);
            for (int i = 0; i < c->topic_count; i++) {
                if (strcmp(c->topics[i], topic) == 0 || strcmp(c->topics[i], "*") == 0) {
                    subscribed = 1;
                    break;
                }
            }
            pthread_mutex_unlock(&c->lock);

            if (subscribed) {
                if (send_ws_frame(c->fd, frame) == 0) {
                    sent_count++;
                }
            }
        }
        c = c->next;
    }
    pthread_mutex_unlock(&g_ws_lock);

    LOG_DEBUG_T("WebSocket", "Broadcast", "OK", "sent to %d clients", sent_count);
    return sent_count;
}

/* ============================================================
 * FTF[WebSocket 服务器主循环]
 * ============================================================ */
static void* ws_loop(void *arg) {
    (void)arg;
    LOG_INFO_T("WebSocket", "Loop", "Start", "WebSocket server started on port %d", WEBSOCKET_PORT);

    int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd < 0) {
        LOG_ERROR_T("WebSocket", "Loop", "SocketFail", "%s", strerror(errno));
        return NULL;
    }

    int opt = 1;
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(WEBSOCKET_PORT);

    if (bind(listen_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        LOG_ERROR_T("WebSocket", "Loop", "BindFail", "%s", strerror(errno));
        close(listen_fd);
        return NULL;
    }

    if (listen(listen_fd, 5) < 0) {
        LOG_ERROR_T("WebSocket", "Loop", "ListenFail", "%s", strerror(errno));
        close(listen_fd);
        return NULL;
    }

    g_ws_running = 1;

    while (g_ws_running) {
        fd_set readfds;
        FD_ZERO(&readfds);
        FD_SET(listen_fd, &readfds);

        struct timeval tv;
        tv.tv_sec = 1;
        tv.tv_usec = 0;

        if (select(listen_fd + 1, &readfds, NULL, NULL, &tv) < 0) {
            if (errno == EINTR) continue;
            LOG_ERROR_T("WebSocket", "Loop", "SelectFail", "%s", strerror(errno));
            break;
        }

        if (!FD_ISSET(listen_fd, &readfds)) continue;

        struct sockaddr_in client_addr;
        socklen_t addr_len = sizeof(client_addr);
        int client_fd = accept(listen_fd, (struct sockaddr*)&client_addr, &addr_len);
        if (client_fd < 0) continue;

        char ip[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &client_addr.sin_addr, ip, sizeof(ip));
        LOG_DEBUG_T("WebSocket", "Loop", "Connect", "connection from %s", ip);

        /* 在独立线程中处理客户端（带 IP——no_verify 免验证检查） */
        ws_thread_arg_t *arg = malloc(sizeof(ws_thread_arg_t));
        if (arg) {
            arg->fd = client_fd;
            safe_strncpy(arg->ip, ip, sizeof(arg->ip));
            pthread_t client_thread;
            pthread_create(&client_thread, NULL, (void* (*)(void*))handle_client, arg);
            pthread_detach(client_thread);
        } else {
            close(client_fd);
        }
    }

    close(listen_fd);
    g_ws_running = 0;
    LOG_INFO_T("WebSocket", "Loop", "Stop", "WebSocket server stopped");
    return NULL;
}

/* ============================================================
 * FTF[启动 WebSocket 服务器]
 * ============================================================ */
int websocket_server_start(void) {
    LOG_INFO_T("WebSocket", "Start", "Enter", "starting WebSocket server");

    if (g_ws_running) {
        LOG_WARN_T("WebSocket", "Start", "AlreadyRunning", "server already running");
        return 0;
    }

    if (pthread_create(&g_ws_thread, NULL, ws_loop, NULL) != 0) {
        LOG_ERROR_T("WebSocket", "Start", "ThreadFail", "pthread_create failed");
        return -1;
    }

    return 0;
}

/* ============================================================
 * FTF[停止 WebSocket 服务器]
 * ============================================================ */
void websocket_server_stop(void) {
    if (!g_ws_running) return;
    g_ws_running = 0;
    pthread_join(g_ws_thread, NULL);
    LOG_INFO_T("WebSocket", "Stop", "OK", "WebSocket server stopped");
}

/* ============================================================
 * FTF[广播消息到所有客户端（兼容旧接口）]
 * ============================================================ */
int websocket_broadcast_all(const char *message) {
    return websocket_broadcast("*", message);
}

/* ============================================================
 * FTF[获取客户端数量]
 * ============================================================ */
int websocket_client_count(void) {
    int count = 0;
    pthread_mutex_lock(&g_ws_lock);
    ws_client_t *c = g_clients;
    while (c) {
        if (c->active) count++;
        c = c->next;
    }
    pthread_mutex_unlock(&g_ws_lock);
    return count;
}