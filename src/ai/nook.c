/**
 * @file    nook.c
 * @brief   Nook主AI通信模块（支持流式/非流式模式，错误码处理）
 * @version LN-B-5.0.0.0
 * @changes 适配新JSON响应（解析thinking/tool_calls字段）；安全字符串替换；双文支持
 */

#include "nook.h"
#include "nook_repair.h"
#include "sandbox.h"
#include "ipc_core.h"
#include "string_no_sys.h"
#include "log_extra.h"
#include "uart.h"
#include "timer.h"
#include "libling.h"
#include "audit.h"
#include "data_path.h"
#include "ai_server_protocol.h"
#include "ai_config.h"
#include "cJSON.h"
#include "safe_string.h"
#include "lang.h"
#include "markdown_renderer.h"
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/time.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
/* nook.c include 区补 sys/time.h（musl 需显式） */
#include <ctype.h>
#include <time.h>
#include <poll.h>

#define MAX_TASKS           16
#define MAX_AUTHORIZATIONS  8

/* 颜色定义（与Python端保持一致） */
#define COLOR_THINKING "\033[90m"   /* 暗灰色 */
#define COLOR_TOOL     "\033[36m"   /* 青色 */
#define COLOR_ERROR    "\033[31m"   /* 红色 */
#define COLOR_RESET    "\033[0m"

/* 流式输出开关（1=直接输出到终端，0=缓存返回） */
int nook_stream_output = 1;

static nook_mode_t nook_current_mode = NOOK_MODE_NORMAL;
static nook_task_t task_table[MAX_TASKS];
static uint32_t task_count = 0;
static nook_authorization_t auth_table[MAX_AUTHORIZATIONS];
static uint32_t auth_count = 0;
static nook_personality_t *current_personality = NULL;

static nook_personality_t nook_default = {
    .name           = "诺克 (Nook)",
    .gender         = GENDER_MALE,
    .voice_tone     = VOICE_MALE,
    .style_strict   = 3,
    .style_warmth   = 3,
    .style_speed    = 4
};

/* ============================================================
 * 内部辅助：清理输入字符串（保留有效字符）
 * ============================================================ */
static void clean_prompt(char *str) {
    LOG_DEBUG_T("Nook", "CleanPrompt", "Enter", "str=%p", (void*)str);
    if (!str) {
        LOG_WARN_T("Nook", "CleanPrompt", "Invalid", "str is NULL");
        return;
    }

    char *src = str;
    char *dst = str;
    while (*src) {
        unsigned char c = (unsigned char)*src;
        if (c >= 0x20 && c <= 0x7E) {
            *dst++ = *src++;
            continue;
        }
        if (c >= 0xE0 && c <= 0xEF && src[1] && src[2]) {
            *dst++ = *src++;
            *dst++ = *src++;
            *dst++ = *src++;
            continue;
        }
        if (c == '\n' || c == '\r') {
            *dst++ = *src++;
            continue;
        }
        src++;
    }
    *dst = '\0';
    LOG_DEBUG_T("Nook", "CleanPrompt", "Exit", "cleaned length=%zu", strlen(str));
}

/* ============================================================
 * 内部辅助：连接 AI 服务器
 * ============================================================ */
static int connect_ai_server(void) {
    LOG_DEBUG_T("Nook", "Connect", "Enter", "attempting connection to %s", AI_SOCKET_PATH);

    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        LOG_ERROR_T("Nook", "Connect", "SocketFail", "socket() error: %s (errno=%d)", strerror(errno), errno);
        return -1;
    }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    safe_strncpy(addr.sun_path, AI_SOCKET_PATH, sizeof(addr.sun_path));
    addr.sun_path[sizeof(addr.sun_path)-1] = '\0';

    if (connect(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        LOG_ERROR_T("Nook", "Connect", "ConnectFail", "connect to %s failed: %s (errno=%d)", AI_SOCKET_PATH, strerror(errno), errno);
        close(fd);
        return -1;
    }

    LOG_DEBUG_T("Nook", "Connect", "OK", "connected, fd=%d", fd);
    return fd;
}

/* ============================================================
 * 内部辅助：带超时的行读取
 * ============================================================ */
static int read_line_timeout(int fd, char *buf, size_t buf_size, int timeout_sec) {
    LOG_DEBUG_T("Nook", "ReadLine", "Enter", "fd=%d, buf_size=%zu, timeout=%d", fd, buf_size, timeout_sec);

    if (!buf || buf_size == 0) {
        LOG_ERROR_T("Nook", "ReadLine", "Invalid", "buf=%p, buf_size=%zu", (void*)buf, buf_size);
        return -1;
    }

    struct timeval tv;
    tv.tv_sec = timeout_sec;
    tv.tv_usec = 0;
    if (setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) < 0) {
        LOG_WARN_T("Nook", "ReadLine", "SetSockOptFail", "setsockopt failed: %s (errno=%d)", strerror(errno), errno);
    }

    size_t pos = 0;
    while (pos < buf_size - 1) {
        ssize_t n = read(fd, buf + pos, 1);
        if (n == -1) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                LOG_WARN_T("Nook", "ReadLine", "Timeout", "read timeout after %d sec", timeout_sec);
                return -2;
            }
            LOG_ERROR_T("Nook", "ReadLine", "ReadFail", "read error: %s (errno=%d)", strerror(errno), errno);
            return -1;
        }
        if (n == 0) {
            LOG_DEBUG_T("Nook", "ReadLine", "EOF", "connection closed by peer, pos=%zu", pos);
            if (pos > 0) {
                buf[pos] = '\0';
                return (int)pos;
            }
            return 0;
        }
        if (buf[pos] == '\n') {
            buf[pos] = '\0';
            LOG_DEBUG_T("Nook", "ReadLine", "OK", "read %zu bytes", pos);
            return (int)pos;
        }
        pos++;
    }
    buf[pos] = '\0';
    LOG_WARN_T("Nook", "ReadLine", "BufferFull", "buffer full, pos=%zu", pos);
    return (int)pos;
}

/* ============================================================
 * 内部辅助：判断是否使用颜色
 * ============================================================ */
static int use_color(void) {
    const ai_config_t *cfg = ai_config_get();
    if (cfg && cfg->stream_style[0] != '\0') {
        int result = (strcmp(cfg->stream_style, "plain") != 0);
        LOG_DEBUG_T("Nook", "UseColor", "cfg->stream_style='%s', result=%d", cfg->stream_style, result);
        return result;
    }
    LOG_DEBUG_T("Nook", "UseColor", "default", "using default (color enabled)");
    return 1;
}

/* ============================================================
 * 内部辅助：发送简单命令到 AI Server
 * ============================================================ */
static int send_command_to_ai_server(const char *cmd_json, char *resp_buf, size_t buf_len) {
    LOG_DEBUG_T("Nook", "SendCmd", "Enter", "cmd_json='%s', buf_len=%zu", cmd_json ? cmd_json : "(null)", buf_len);

    if (!cmd_json || !resp_buf || buf_len == 0) {
        LOG_ERROR_T("Nook", "SendCmd", "Invalid", "cmd_json=%p, resp_buf=%p, buf_len=%zu", (void*)cmd_json, (void*)resp_buf, buf_len);
        return -1;
    }

    int fd = connect_ai_server();
    if (fd < 0) {
        LOG_ERROR_T("Nook", "SendCmd", "ConnectFail", "cannot connect to AI server");
        safe_snprintf(resp_buf, buf_len, tr("Error: Cannot connect to AI server", "错误：无法连接到 AI 服务器"));
        return -1;
    }

    ssize_t written = write(fd, cmd_json, strlen(cmd_json));
    if (written < 0) {
        LOG_ERROR_T("Nook", "SendCmd", "WriteFail", "write error: %s (errno=%d)", strerror(errno), errno);
        close(fd);
        safe_snprintf(resp_buf, buf_len, tr("Error: Write failed", "错误：写入失败"));
        return -1;
    }
    if (write(fd, "\n", 1) < 0) {
        LOG_ERROR_T("Nook", "SendCmd", "NewlineFail", "write newline error: %s", strerror(errno));
        close(fd);
        safe_snprintf(resp_buf, buf_len, tr("Error: Write failed", "错误：写入失败"));
        return -1;
    }
    LOG_DEBUG_T("Nook", "SendCmd", "Written", "sent %zd bytes", written);

    int ret = read_line_timeout(fd, resp_buf, buf_len, 10);
    close(fd);

    if (ret <= 0) {
        LOG_WARN_T("Nook", "SendCmd", "ReadFail", "read_line_timeout returned %d", ret);
        safe_snprintf(resp_buf, buf_len, tr("Error: No response from AI server", "错误：AI 服务器无响应"));
        return -1;
    }

    LOG_DEBUG_T("Nook", "SendCmd", "OK", "response='%s'", resp_buf);
    return 0;
}

/* ============================================================
 * 内部辅助：Unicode 转义解码
 * ============================================================ */
static char* unescape_string(const char *src, char *dst, size_t dst_size) {
    LOG_DEBUG_T("Nook", "Unescape", "Enter", "src_len=%zu, dst_size=%zu", src ? strlen(src) : 0, dst_size);

    if (!src || !dst) {
        LOG_ERROR_T("Nook", "Unescape", "Invalid", "src=%p, dst=%p", (void*)src, (void*)dst);
        return NULL;
    }

    char *dst_ptr = dst;
    const char *src_ptr = src;
    size_t remaining = dst_size - 1;

    while (*src_ptr && remaining > 0) {
        if (*src_ptr == '\\' && *(src_ptr + 1)) {
            src_ptr++;
            switch (*src_ptr) {
                case 'n':  *dst_ptr++ = '\n'; break;
                case 't':  *dst_ptr++ = '\t'; break;
                case 'r':  *dst_ptr++ = '\r'; break;
                case '"':  *dst_ptr++ = '"';  break;
                case '\\': *dst_ptr++ = '\\'; break;
                case 'u': {
                    unsigned int code = 0;
                    for (int i = 0; i < 4; i++) {
                        char c = *(src_ptr + 1 + i);
                        if (c >= '0' && c <= '9') code = (code << 4) | (c - '0');
                        else if (c >= 'a' && c <= 'f') code = (code << 4) | (c - 'a' + 10);
                        else if (c >= 'A' && c <= 'F') code = (code << 4) | (c - 'A' + 10);
                        else break;
                    }
                    src_ptr += 4;
                    if (code >= 0x800) {
                        *dst_ptr++ = (char)(0xE0 | (code >> 12));
                        if (remaining >= 2) { *dst_ptr++ = (char)(0x80 | ((code >> 6) & 0x3F)); remaining--; }
                        if (remaining >= 1) { *dst_ptr++ = (char)(0x80 | (code & 0x3F)); remaining--; }
                    } else if (code >= 0x80) {
                        *dst_ptr++ = (char)(0xC0 | (code >> 6));
                        if (remaining >= 1) { *dst_ptr++ = (char)(0x80 | (code & 0x3F)); remaining--; }
                    } else {
                        *dst_ptr++ = (char)code;
                    }
                    remaining--;
                    continue;
                }
                default:
                    *dst_ptr++ = '\\';
                    *dst_ptr++ = *src_ptr;
                    break;
            }
            src_ptr++;
            remaining--;
        } else {
            *dst_ptr++ = *src_ptr++;
        }
        remaining--;
    }
    *dst_ptr = '\0';

    LOG_DEBUG_T("Nook", "Unescape", "Exit", "decoded %zu bytes", strlen(dst));
    return dst;
}

/* ============================================================
 * 【修改】内部辅助：解析响应内容（含 thinking/tool_calls）
 * ============================================================ */
static const char* parse_response_content(const char *json_resp, char *out_buf, size_t out_len,
                                          char *thinking_buf, size_t thinking_len,
                                          char *tool_calls_buf, size_t tool_calls_len) {
    LOG_DEBUG_T("Nook", "ParseResponse", "Enter", "json_len=%zu, out_len=%zu",
                json_resp ? strlen(json_resp) : 0, out_len);

    if (!json_resp || !out_buf || out_len == 0) {
        LOG_ERROR_T("Nook", "ParseResponse", "Invalid", "json_resp=%p, out_buf=%p, out_len=%zu",
                    (void*)json_resp, (void*)out_buf, out_len);
        return out_buf;
    }

    /* 初始化输出缓冲区 */
    if (thinking_buf && thinking_len > 0) thinking_buf[0] = '\0';
    if (tool_calls_buf && tool_calls_len > 0) tool_calls_buf[0] = '\0';

    cJSON *root = cJSON_Parse(json_resp);
    if (!root) {
        LOG_WARN_T("Nook", "ParseResponse", "ParseFail", "invalid JSON: %s", json_resp);
        safe_snprintf(out_buf, out_len, "{\"error\":\"Invalid JSON response\"}");
        return out_buf;
    }

    cJSON *status = cJSON_GetObjectItem(root, "status");
    cJSON *content = cJSON_GetObjectItem(root, "content");
    cJSON *thinking = cJSON_GetObjectItem(root, "thinking");
    cJSON *tool_calls = cJSON_GetObjectItem(root, "tool_calls");
    cJSON *error_code = cJSON_GetObjectItem(root, "error_code");

    /* 【新增】提取思考链 */
    if (thinking_buf && thinking_len > 0 && cJSON_IsString(thinking) && thinking->valuestring) {
        safe_strncpy(thinking_buf, thinking->valuestring, thinking_len);
        thinking_buf[thinking_len - 1] = '\0';
        LOG_DEBUG_T("Nook", "ParseResponse", "Thinking", "thinking='%s'", thinking_buf);
        /* 显示思考链到终端 */
        if (use_color()) {
            uart_puts(COLOR_THINKING);
            uart_puts(tr("╭ Thinking: ", "╭ 思考中："));
            uart_puts(thinking_buf);
            uart_puts(COLOR_RESET);
            uart_puts("\n");
        } else {
            uart_puts(tr("Thinking: ", "思考中："));
            uart_puts(thinking_buf);
            uart_puts("\n");
        }
    }

    /* 【新增】提取工具调用 */
    if (tool_calls_buf && tool_calls_len > 0 && cJSON_IsArray(tool_calls)) {
        char *tc_str = cJSON_PrintUnformatted(tool_calls);
        if (tc_str) {
            safe_strncpy(tool_calls_buf, tc_str, tool_calls_len);
            tool_calls_buf[tool_calls_len - 1] = '\0';
            free(tc_str);
            LOG_DEBUG_T("Nook", "ParseResponse", "ToolCalls", "tool_calls='%s'", tool_calls_buf);
        }
    }

    /* 优先检查 content 字段 */
    if (cJSON_IsString(content) && content->valuestring && content->valuestring[0] != '\0') {
        LOG_DEBUG_T("Nook", "ParseResponse", "Content", "content='%s'", content->valuestring);
        safe_strncpy(out_buf, content->valuestring, out_len);
        out_buf[out_len - 1] = '\0';
        unescape_string(out_buf, out_buf, out_len);
        cJSON_Delete(root);
        LOG_DEBUG_T("Nook", "ParseResponse", "Exit", "returning content (len=%zu)", strlen(out_buf));
        return out_buf;
    }

    /* 检查错误状态 */
    if (cJSON_IsString(status) && strcmp(status->valuestring, "error") == 0) {
        const char *err_msg = tr("Unknown error", "未知错误");
        cJSON *err_content = cJSON_GetObjectItem(root, "content");
        if (cJSON_IsString(err_content) && err_content->valuestring) {
            err_msg = err_content->valuestring;
        }
        int code = 0;
        if (cJSON_IsNumber(error_code)) {
            code = error_code->valueint;
        } else if (cJSON_IsString(error_code)) {
            code = atoi(error_code->valuestring);
        }

        if (code > 0) {
            LOG_WARN_T("Nook", "ParseResponse", "Error", "AI error code=%d, msg='%s'", code, err_msg);
            safe_snprintf(out_buf, out_len, tr("[AI Error] %d %s", "[AI 错误] %d %s"), code, err_msg);
        } else {
            LOG_WARN_T("Nook", "ParseResponse", "Error", "AI error: %s", err_msg);
            safe_snprintf(out_buf, out_len, tr("[AI Error] %s", "[AI 错误] %s"), err_msg);
        }
        cJSON_Delete(root);
        return out_buf;
    }

    /* 未知格式：返回完整JSON（调试用） */
    LOG_WARN_T("Nook", "ParseResponse", "UnknownFormat", "response=%s", json_resp);
    safe_strncpy(out_buf, json_resp, out_len);
    out_buf[out_len - 1] = '\0';
    cJSON_Delete(root);
    return out_buf;
}

/* ============================================================
 * 公共 API 实现
 * ============================================================ */

void nook_init(void) {
    LOG_INFO_T("Nook", "Init", "Enter", "Initializing Nook master AI");
    task_count = 0;
    auth_count = 0;
    nook_current_mode = NOOK_MODE_NORMAL;
    current_personality = &nook_default;

    for (int i = 0; i < MAX_TASKS; i++) {
        task_table[i].task_id = 0;
        task_table[i].status = TASK_STATUS_COMPLETED;
    }
    LOG_INFO_T("Nook", "Init", "OK", "Nook master AI initialized (mode: Normal)");
    uart_puts(tr("[Nook] Master, Nook is ready.\n", "[Nook] 主人，诺克已就绪。\n"));
}

void nook_set_mode(nook_mode_t mode) {
    LOG_INFO_T("Nook", "SetMode", "Enter", "mode=%d", mode);
    nook_current_mode = mode;
    LOG_INFO_T("Nook", "SetMode", "OK", "mode switched to %d", mode);
    uart_puts(tr("[Nook] Mode switched to: ", "[Nook] 模式切换至："));
    uart_puts(nook_mode_str());
    uart_puts("\n");
}

nook_mode_t nook_get_mode(void) {
    LOG_DEBUG_T("Nook", "GetMode", "result", "returning %d", nook_current_mode);
    return nook_current_mode;
}

const char *nook_mode_str(void) {
    const char *s;
    switch (nook_current_mode) {
        case NOOK_MODE_NORMAL:   s = tr("Normal", "日常模式"); break;
        case NOOK_MODE_LOCKDOWN: s = tr("Lockdown", "封锁模式"); break;
        case NOOK_MODE_DANGER:   s = tr("Danger", "危险模式"); break;
        default:                 s = tr("Unknown", "未知"); break;
    }
    LOG_DEBUG_T("Nook", "ModeStr", "convert", "mode=%d -> '%s'", nook_current_mode, s);
    return s;
}

int nook_dispatch_task(const char *d, task_priority_t p, const char *a) {
    LOG_INFO_T("Nook", "DispatchTask", "Enter", "desc='%s', pri=%d, ai='%s'", d ? d : "(null)", p, a ? a : "(null)");

    if (task_count >= MAX_TASKS) {
        LOG_ERROR_T("Nook", "DispatchTask", "Overflow", "task_count=%u >= MAX_TASKS=%d", task_count, MAX_TASKS);
        return -1;
    }

    uint32_t id = task_count + 1;
    task_table[task_count].task_id = id;
    /* 【修复】固定数组复制，避免外部指针悬垂 */
    safe_strncpy(task_table[task_count].desc, d ? d : "(null)", sizeof(task_table[task_count].desc));
    task_table[task_count].pri = p;
    task_table[task_count].status = TASK_STATUS_PENDING;
    safe_strncpy(task_table[task_count].ai, a ? a : "Internal", sizeof(task_table[task_count].ai));
    task_count++;

    LOG_INFO_T("Nook", "DispatchTask", "OK", "task #%u dispatched", id);
    uart_puts(tr("[Nook] Task #", "[Nook] 任务 #"));
    char buf[8];
    int_to_str(id, buf);
    uart_puts(buf);
    uart_puts(tr(" → ", " → "));
    uart_puts(a ? a : tr("Internal", "内部"));
    uart_puts(" : ");
    uart_puts(d ? d : "(null)");
    uart_puts("\n");
    return id;
}

void nook_report_task_result(uint32_t task_id, task_status_t result) {
    LOG_INFO_T("Nook", "ReportResult", "Enter", "task_id=%u, result=%d", task_id, result);

    for (uint32_t i = 0; i < task_count; i++) {
        if (task_table[i].task_id == task_id) {
            task_table[i].status = result;
            LOG_INFO_T("Nook", "ReportResult", "OK", "task #%u status updated to %d", task_id, result);
            uart_puts(tr("[Nook] Task #", "[Nook] 任务 #"));
            char buf[8];
            int_to_str(task_id, buf);
            uart_puts(buf);
            uart_puts(tr(" status: ", " 状态："));
            switch (result) {
                case TASK_STATUS_COMPLETED: uart_puts(tr("Completed", "完成")); break;
                case TASK_STATUS_FAILED:    uart_puts(tr("Failed", "失败")); break;
                case TASK_STATUS_ABORTED:   uart_puts(tr("Aborted", "已中止")); break;
                default: uart_puts(tr("Unknown", "未知")); break;
            }
            uart_puts("\n");
            return;
        }
    }
    LOG_WARN_T("Nook", "ReportResult", "NotFound", "task #%u not found", task_id);
}

int nook_authorize_ai(const char *n, uint32_t perms, uint32_t tid, uint8_t cross) {
    LOG_INFO_T("Nook", "AuthorizeAI", "Enter", "ai='%s', perms=0x%08X, task=%u, cross=%d",
               n ? n : "(null)", perms, tid, cross);

    if (!n || auth_count >= MAX_AUTHORIZATIONS) {
        LOG_ERROR_T("Nook", "AuthorizeAI", "Invalid", "n=%p, auth_count=%u", (void*)n, auth_count);
        return -1;
    }

    /* 【修复】固定数组复制，避免外部指针悬垂；TTL 默认永久（0） */
    safe_strncpy(auth_table[auth_count].ai_name, n, sizeof(auth_table[auth_count].ai_name));
    auth_table[auth_count].perms = perms;
    auth_table[auth_count].task_id = tid;
    auth_table[auth_count].cross = cross;
    auth_table[auth_count].granted_at = time(NULL);
    auth_table[auth_count].duration_sec = 0;   /* 0 = 永久（可由 TTL 接口扩展） */
    auth_count++;

    LOG_INFO_T("Nook", "AuthorizeAI", "OK", "authorized AI '%s'", n);
    uart_puts(tr("[Nook] Authorized sub-AI: ", "[Nook] 授权子AI："));
    uart_puts(n);
    uart_puts(tr(" (Task #", " (任务 #"));
    char buf[8];
    int_to_str(tid, buf);
    uart_puts(buf);
    if (cross) uart_puts(tr(", cross-task", ", 跨任务)"));
    uart_puts(tr(")", ")"));
    uart_puts("\n");
    return 0;
}

int nook_revoke_ai(const char *n) {
    LOG_INFO_T("Nook", "RevokeAI", "Enter", "ai='%s'", n ? n : "(null)");

    if (!n) {
        LOG_ERROR_T("Nook", "RevokeAI", "Invalid", "n is NULL");
        return -1;
    }

    for (uint32_t i = 0; i < auth_count; i++) {
        if (strcmp(auth_table[i].ai_name, n) == 0) {
            auth_table[i].perms = 0;
            LOG_INFO_T("Nook", "RevokeAI", "OK", "revoked AI '%s'", n);
            uart_puts(tr("[Nook] Revoked ", "[Nook] 撤销 "));
            uart_puts(n);
            uart_puts(tr(" permissions", " 权限"));
            uart_puts("\n");
            return 0;
        }
    }

    LOG_WARN_T("Nook", "RevokeAI", "NotFound", "AI '%s' not found", n);
    return -1;
}

int nook_check_authorization(const char *n, uint32_t perm) {
    LOG_DEBUG_T("Nook", "CheckAuth", "Enter", "ai='%s', perm=0x%08X", n ? n : "(null)", perm);

    if (!n) {
        LOG_WARN_T("Nook", "CheckAuth", "Invalid", "n is NULL");
        return 0;
    }

    for (uint32_t i = 0; i < auth_count; i++) {
        if (strcmp(auth_table[i].ai_name, n) == 0 && (auth_table[i].perms & perm)) {
            /* 【安全冗余】检查授权是否过期 */
            if (auth_table[i].duration_sec > 0) {
                time_t now = time(NULL);
                if (now - auth_table[i].granted_at > auth_table[i].duration_sec) {
                    LOG_DEBUG_T("Nook", "CheckAuth", "Expired", "AI '%s' auth expired", n);
                    return 0;
                }
            }
            LOG_DEBUG_T("Nook", "CheckAuth", "OK", "AI '%s' authorized for perm 0x%08X", n, perm);
            return 1;
        }
    }

    LOG_DEBUG_T("Nook", "CheckAuth", "Denied", "AI '%s' not authorized for perm 0x%08X", n, perm);
    return 0;
}

void nook_set_personality(nook_personality_t *p) {
    LOG_INFO_T("Nook", "SetPersonality", "Enter", "p=%p", (void*)p);

    if (p) {
        current_personality = p;
        LOG_INFO_T("Nook", "SetPersonality", "OK", "personality switched to '%s'", p->name);
        uart_puts(tr("[Nook] Personality switched to: ", "[Nook] 人格切换至："));
        uart_puts(p->name);
        uart_puts("\n");
    } else {
        LOG_WARN_T("Nook", "SetPersonality", "Invalid", "p is NULL, keeping current");
    }
}

nook_personality_t *nook_get_personality(void) {
    LOG_DEBUG_T("Nook", "GetPersonality", "returning %p", (void*)current_personality);
    return current_personality;
}

void nook_set_user_name(const char *name) {
    LOG_INFO_T("Nook", "SetUserName", "Enter", "name='%s'", name ? name : "(null)");

    if (!name || !*name) {
        LOG_WARN_T("Nook", "SetUserName", "Invalid", "name is empty");
        uart_puts(tr("[Nook] Name cannot be empty.\n", "[Nook] 称呼不能为空。\n"));
        return;
    }

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "cmd", "set_user_name");
    cJSON_AddStringToObject(root, "name", name);
    char *json_str = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);

    if (!json_str) {
        LOG_ERROR_T("Nook", "SetUserName", "JSONFail", "cJSON_PrintUnformatted failed");
        uart_puts(tr("[Nook] Internal error: cannot construct request.\n",
                    "[Nook] 内部错误：无法构造请求。\n"));
        return;
    }

    LOG_DEBUG_T("Nook", "SetUserName", "Sending", "json='%s'", json_str);
    char resp_buf[512];
    int ret = send_command_to_ai_server(json_str, resp_buf, sizeof(resp_buf));
    free(json_str);

    if (ret == 0) {
        cJSON *resp_root = cJSON_Parse(resp_buf);
        if (resp_root) {
            cJSON *status = cJSON_GetObjectItem(resp_root, "status");
            cJSON *content = cJSON_GetObjectItem(resp_root, "content");
            if (status && cJSON_IsString(status) && strcmp(status->valuestring, "ok") == 0) {
                uart_puts(tr("[Nook] ", "[Nook] "));
                if (content && cJSON_IsString(content)) {
                    uart_puts(content->valuestring);
                } else {
                    uart_puts(tr("Name updated.", "称呼已更新。"));
                }
                uart_puts("\n");
                LOG_INFO_T("Nook", "SetUserName", "OK", "name updated successfully");
            } else {
                LOG_WARN_T("Nook", "SetUserName", "Error", "server returned error: %s", resp_buf);
                uart_puts(tr("[Nook] Failed to update name: ", "[Nook] 更新称呼失败："));
                if (content && cJSON_IsString(content)) {
                    uart_puts(content->valuestring);
                } else {
                    uart_puts(tr("Unknown error", "未知错误"));
                }
                uart_puts("\n");
            }
            cJSON_Delete(resp_root);
        } else {
            LOG_WARN_T("Nook", "SetUserName", "ParseFail", "invalid response: %s", resp_buf);
            uart_puts(tr("[Nook] Cannot parse server response.\n",
                        "[Nook] 无法解析服务器响应。\n"));
        }
    } else {
        LOG_ERROR_T("Nook", "SetUserName", "SendFail", "send_command_to_ai_server returned %d", ret);
        uart_puts(tr("[Nook] Cannot connect to AI service, name not updated.\n",
                    "[Nook] 无法连接到 AI 服务，称呼未更新。\n"));
    }
}

const char* nook_get_user_name(void) {
    LOG_DEBUG_T("Nook", "GetUserName", "returning default (deprecated)", "");
    return tr("Sir", "先生");
}

void nook_show_status(void) {
    LOG_DEBUG_T("Nook", "ShowStatus", "Enter", "");
    uart_puts(tr("=== Nook Status ===\n", "=== Nook 状态 ===\n"));
    uart_puts(tr("Mode: ", "模式："));
    uart_puts(nook_mode_str());
    uart_puts("\n");
    uart_puts(tr("Tasks: ", "任务数："));
    char buf[8];
    int_to_str(task_count, buf);
    uart_puts(buf);
    uart_puts("\n");
    uart_puts(tr("Authorizations: ", "授权数："));
    int_to_str(auth_count, buf);
    uart_puts(buf);
    uart_puts("\n");
    LOG_DEBUG_T("Nook", "ShowStatus", "Exit", "tasks=%u, auths=%u", task_count, auth_count);
}

/* ============================================================
 * 【修改】核心：nook_ask_ollama（支持 thinking/tool_calls 解析）
 * ============================================================ */
int nook_ask_ollama(const char *prompt, const char *model, char *resp, uint32_t len, int timeout_sec) {
    LOG_INFO_T("Nook", "Ask", "Enter", "prompt='%.100s...', model='%s', len=%u, timeout=%d",
               prompt ? prompt : "(null)", model ? model : "(null)", len, timeout_sec);

    if (!prompt || !resp || len == 0) {
        LOG_ERROR_T("Nook", "Ask", "Invalid", "prompt=%p, resp=%p, len=%u", (void*)prompt, (void*)resp, len);
        return -1;
    }

    char cleaned_prompt[2048];
    safe_strncpy(cleaned_prompt, prompt, sizeof(cleaned_prompt));
    cleaned_prompt[sizeof(cleaned_prompt)-1] = '\0';
    clean_prompt(cleaned_prompt);
    LOG_DEBUG_T("Nook", "Ask", "Cleaned", "prompt cleaned length=%zu", strlen(cleaned_prompt));

    int fd = connect_ai_server();
    if (fd < 0) {
        LOG_ERROR_T("Nook", "Ask", "ConnectFail", "cannot connect to AI server");
        safe_snprintf(resp, len, tr("AI server not available.", "AI 服务器不可用。"));
        return -3;
    }

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "cmd", AI_CMD_NOOK_ASK);
    cJSON_AddStringToObject(root, "prompt", cleaned_prompt);
    int timeout = (timeout_sec > 0) ? timeout_sec : ai_config_get()->socket_timeout;
    cJSON_AddNumberToObject(root, "timeout", timeout);
    if (model && model[0] != '\0') {
        cJSON_AddStringToObject(root, "model", model);
    }
    /* 【新增】请求显示控制参数 */
    cJSON_AddBoolToObject(root, "show_thinking", 1);
    cJSON_AddBoolToObject(root, "show_tool_calls", 1);
    cJSON_AddBoolToObject(root, "show_tool_results", 1);

    char *msg = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);

    if (!msg) {
        LOG_ERROR_T("Nook", "Ask", "JSONFail", "cJSON_PrintUnformatted failed");
        close(fd);
        safe_snprintf(resp, len, tr("Failed to construct request", "构造请求失败"));
        return -1;
    }

    LOG_DEBUG_T("Nook", "Ask", "Sending", "msg='%s'", msg);

    ssize_t written = write(fd, msg, strlen(msg));
    if (written < 0) {
        LOG_ERROR_T("Nook", "Ask", "SendFail", "write error: %s (errno=%d)", strerror(errno), errno);
        free(msg);
        close(fd);
        safe_snprintf(resp, len, tr("Send failed", "发送失败"));
        return -1;
    }
    if (write(fd, "\n", 1) < 0) {
        LOG_ERROR_T("Nook", "Ask", "NewlineFail", "write newline error: %s", strerror(errno));
        free(msg);
        close(fd);
        safe_snprintf(resp, len, tr("Send failed", "发送失败"));
        return -1;
    }
    free(msg);
    LOG_DEBUG_T("Nook", "Ask", "Sent", "sent %zd bytes", written);

    char line_buf[AI_RECV_BUF_SIZE];
    int ret = read_line_timeout(fd, line_buf, sizeof(line_buf), timeout);
    close(fd);

    if (ret <= 0) {
        LOG_WARN_T("Nook", "Ask", "ReadFail", "read_line_timeout returned %d", ret);
        if (ret == -2) {
            safe_snprintf(resp, len, tr("[AI Error] Timeout (%ds)", "[AI 错误] 超时 (%d秒)"), timeout);
            return -2;
        }
        safe_snprintf(resp, len, tr("AI server returned empty response", "AI 服务器返回空响应"));
        return -1;
    }

    LOG_DEBUG_T("Nook", "Ask", "Response", "raw response='%s'", line_buf);

    /* 【修改】解析响应（含 thinking/tool_calls） */
    char thinking_buf[4096];
    char tool_calls_buf[4096];
    char parsed_resp[len];
    const char *final = parse_response_content(line_buf, parsed_resp, len,
                                                thinking_buf, sizeof(thinking_buf),
                                                tool_calls_buf, sizeof(tool_calls_buf));

    /* 检查是否包含错误前缀 */
    if (strncmp(final, "[AI Error]", 10) == 0) {
        LOG_WARN_T("Nook", "Ask", "Error", "AI returned error: %s", final);
    }

    /* 【新增】如果解析到工具调用，显示工具调用信息 */
    if (tool_calls_buf[0] != '\0') {
        LOG_DEBUG_T("Nook", "Ask", "ToolCalls", "tool_calls=%s", tool_calls_buf);
        if (use_color()) {
            uart_puts(COLOR_TOOL);
            uart_puts(tr("▸ Tool calls: ", "▸ 工具调用："));
            uart_puts(tool_calls_buf);
            uart_puts(COLOR_RESET);
            uart_puts("\n");
        } else {
            uart_puts(tr("Tool calls: ", "工具调用："));
            uart_puts(tool_calls_buf);
            uart_puts("\n");
        }
    }

    /* 【批次F】结构化事件显示（tool_call/tool_result，跳过 thinking 避免重复） */
    cJSON *evt_root = cJSON_Parse(line_buf);
    if (evt_root) {
        cJSON *events = cJSON_GetObjectItem(evt_root, "events");
        if (cJSON_IsArray(events)) {
            int evt_n = cJSON_GetArraySize(events);
            for (int i = 0; i < evt_n; i++) {
                cJSON *e = cJSON_GetArrayItem(events, i);
                cJSON *etype = cJSON_GetObjectItem(e, "type");
                if (!cJSON_IsString(etype)) continue;
                const char *et = etype->valuestring;
                cJSON *ename = cJSON_GetObjectItem(e, "name");
                cJSON *econtent = cJSON_GetObjectItem(e, "content");
                cJSON *eargs = cJSON_GetObjectItem(e, "args");

                if (strcmp(et, "tool_call") == 0 && cJSON_IsString(ename)) {
                    uart_puts(COLOR_TOOL);
                    uart_puts(tr("▸ ", "▸ "));
                    uart_puts(ename->valuestring);
                    if (cJSON_IsString(eargs)) {
                        uart_puts(" ");
                        uart_puts(eargs->valuestring);
                    }
                    uart_puts(COLOR_RESET);
                    uart_puts("\n");
                } else if (strcmp(et, "tool_result") == 0 && cJSON_IsString(ename)) {
                    cJSON *esuccess = cJSON_GetObjectItem(e, "success");
                    int ok = esuccess && cJSON_IsNumber(esuccess) && esuccess->valueint;
                    uart_puts(ok ? COLOR_GREEN : COLOR_RED);
                    uart_puts(ok ? "✓ " : "✗ ");
                    uart_puts(ename->valuestring);
                    uart_puts(COLOR_RESET);
                    if (cJSON_IsString(econtent)) {
                        uart_puts(" ");
                        uart_puts(econtent->valuestring);
                    }
                    uart_puts("\n");
                }
            }
        }
        cJSON_Delete(evt_root);
    }

    /* 【批次F】富文本渲染最终回复（markdown → ANSI，OpenCode 风格无前缀） */
    if (strncmp(final, "[AI Error]", 10) != 0 && final[0] != '\0') {
        uart_puts("\n");
        md_render_text(final);
        uart_puts("\n");
    }

    safe_strncpy(resp, final, len);
    resp[len - 1] = '\0';

    LOG_INFO_T("Nook", "Ask", "Exit", "response length=%zu", strlen(resp));
    return 0;
}

/* ============================================================
 * FTF[查询 auth.sock 是否有待授权请求（流式期间轮询，解决 Shell 阻塞无法授权）]
 * ============================================================ */
static int nook_auth_pending(char *request_id, size_t id_len) {
    if (!request_id || id_len == 0) return 0;
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return 0;

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    safe_strncpy(addr.sun_path, "/LINGOS/run/auth.sock", sizeof(addr.sun_path));
    addr.sun_path[sizeof(addr.sun_path)-1] = '\0';

    if (connect(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        close(fd);
        return 0;
    }
    const char *query = "{\"cmd\":\"pending\"}\n";
    if (write(fd, query, strlen(query)) < 0) {
        close(fd);
        return 0;
    }
    char buf[256];
    int n = (int)read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0) return 0;
    buf[n] = '\0';

    const char *p = strstr(buf, "\"request_id\"");
    if (!p) return 0;
    p = strchr(p, ':');
    if (!p) return 0;
    p++;
    while (*p == ' ' || *p == '\t') p++;
    if (*p != '"') return 0;
    p++;
    int i = 0;
    while (*p && *p != '"' && i < (int)id_len - 1) {
        request_id[i++] = *p++;
    }
    request_id[i] = '\0';
    return (i > 0) ? 1 : 0;
}

/* ============================================================
 * FTF[响应授权请求（approved=1 批准 / 0 拒绝）]
 * ============================================================ */
static void nook_auth_respond(const char *request_id, int approved) {
    if (!request_id || !*request_id) return;
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return;
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    safe_strncpy(addr.sun_path, "/LINGOS/run/auth.sock", sizeof(addr.sun_path));
    addr.sun_path[sizeof(addr.sun_path)-1] = '\0';
    if (connect(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        close(fd);
        return;
    }
    char req[256];
    safe_snprintf(req, sizeof(req), "{\"cmd\":\"respond\",\"request_id\":\"%s\",\"decision\":\"%s\"}\n",
                  request_id, approved ? "approve" : "deny");
    write(fd, req, strlen(req));
    close(fd);
}

/* ============================================================
 * FTF[显示子 AI 视图（^N 切换：新连接查询 agent_view，避免与流式冲突）]
 * ============================================================ */
static void display_agent_view(int index) {
    int vfd = connect_ai_server();
    if (vfd < 0) return;

    char req[64];
    safe_snprintf(req, sizeof(req), "{\"cmd\":\"agent_view\",\"index\":%d}", index);
    write(vfd, req, strlen(req));
    write(vfd, "\n", 1);

    char resp[8192];
    if (read_line_timeout(vfd, resp, sizeof(resp), 5) > 0) {
        cJSON *root = cJSON_Parse(resp);
        if (root) {
            cJSON *agent = cJSON_GetObjectItem(root, "agent");
            cJSON *status = cJSON_GetObjectItem(root, "status");
            cJSON *round = cJSON_GetObjectItem(root, "round");
            cJSON *maxr = cJSON_GetObjectItem(root, "max_rounds");
            cJSON *result = cJSON_GetObjectItem(root, "result");
            cJSON *dialogue = cJSON_GetObjectItem(root, "dialogue");

            uart_puts(COLOR_CYAN);
            uart_puts("\n┌──────────────────────────────────────────────┐\n");
            char buf[256];
            safe_snprintf(buf, sizeof(buf), "│ %s %s  (%s)  |  round %d/%d\n",
                          tr("Sub-Agent View:", "子AI 视图："),
                          cJSON_IsString(agent) ? agent->valuestring : "?",
                          cJSON_IsString(status) ? status->valuestring : "?",
                          round && cJSON_IsNumber(round) ? round->valueint : 0,
                          maxr && cJSON_IsNumber(maxr) ? maxr->valueint : 20);
            uart_puts(buf);
            uart_puts(COLOR_RESET);
            uart_puts("└──────────────────────────────────────────────┘\n");

            /* 对话记录 */
            if (cJSON_IsArray(dialogue)) {
                int n = cJSON_GetArraySize(dialogue);
                for (int i = 0; i < n && i < 8; i++) {
                    cJSON *d = cJSON_GetArrayItem(dialogue, i);
                    cJSON *dt = cJSON_GetObjectItem(d, "type");
                    cJSON *dc = cJSON_GetObjectItem(d, "content");
                    if (cJSON_IsString(dt) && cJSON_IsString(dc) && dc->valuestring) {
                        uart_puts("  ");
                        uart_puts(dt->valuestring);
                        uart_puts(": ");
                        uart_puts(dc->valuestring);
                        uart_puts("\n");
                    }
                }
            }

            /* 结果 */
            if (cJSON_IsString(result) && result->valuestring && result->valuestring[0]) {
                uart_puts(COLOR_GREEN);
                uart_puts(tr("  Result: ", "  结果："));
                uart_puts(COLOR_RESET);
                uart_puts(result->valuestring);
                uart_puts("\n");
            }
            uart_puts(tr("  (press 0 to return to main AI)\n", "  （按 0 返回主 AI）\n"));
            cJSON_Delete(root);
        }
    }
    close(vfd);
}

/* ============================================================
 * 【批次F增强】流式 AI 对话（过程事件 + 最终回复逐块实时显示）
 * ============================================================ */
int nook_ask_stream(const char *prompt, const char *model, char *resp, uint32_t len, int timeout_sec) {
    LOG_INFO_T("Nook", "AskStream", "Enter", "prompt='%.80s...'", prompt ? prompt : "(null)");
    if (!prompt || !resp || len == 0) {
        LOG_ERROR_T("Nook", "AskStream", "Invalid", "prompt=%p, resp=%p, len=%u",
                    (void*)prompt, (void*)resp, len);
        return -1;
    }

    char cleaned_prompt[2048];
    safe_strncpy(cleaned_prompt, prompt, sizeof(cleaned_prompt));
    cleaned_prompt[sizeof(cleaned_prompt)-1] = '\0';
    clean_prompt(cleaned_prompt);

    int fd = connect_ai_server();
    if (fd < 0) {
        LOG_ERROR_T("Nook", "AskStream", "ConnectFail", "cannot connect to AI server");
        safe_snprintf(resp, len, tr("AI server not available.", "AI 服务器不可用。"));
        return -3;
    }

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "cmd", "nook_ask_stream");
    cJSON_AddStringToObject(root, "prompt", cleaned_prompt);
    int timeout = (timeout_sec > 0) ? timeout_sec : ai_config_get()->socket_timeout;
    cJSON_AddNumberToObject(root, "timeout", timeout);
    if (model && model[0] != '\0') {
        cJSON_AddStringToObject(root, "model", model);
    }
    char *msg = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!msg) { close(fd); return -1; }
    write(fd, msg, strlen(msg));
    write(fd, "\n", 1);
    free(msg);

    /* 循环读取流式事件行（poll 双监听：ai.sock 事件 + stdin ^N 切换键） */
    struct pollfd pfds[2];
    pfds[0].fd = fd;
    pfds[0].events = POLLIN;
    pfds[0].revents = 0;
    pfds[1].fd = STDIN_FILENO;
    pfds[1].events = POLLIN;
    pfds[1].revents = 0;

    char line_buf[AI_RECV_BUF_SIZE];
    size_t line_pos = 0;
    char full_buf[65536];
    size_t full_len = 0;
    full_buf[0] = '\0';
    int finished = 0;
    int thinking_active = 0;   /* 【优化1】思考流式状态（结束时换行分隔） */
    time_t req_start = time(NULL);   /* 【状态行】请求开始时间 */
    /* 【修复3】流式期间授权轮询（Shell 阻塞时仍可响应高风险授权） */
    time_t last_auth_check = 0;
    int auth_pending_active = 0;
    char auth_req_id[64] = {0};

    while (!finished) {
        int pr = poll(pfds, 2, 500);
        if (pr < 0) {
            if (errno == EINTR) continue;
            break;
        }

        /* ---- AI 事件 ---- */
        if (pfds[0].revents & POLLIN) {
            ssize_t n = read(fd, line_buf + line_pos, 1);
            if (n <= 0) break;
            if (line_buf[line_pos] == '\n') {
                line_buf[line_pos] = '\0';
                line_pos = 0;

                cJSON *evt = cJSON_Parse(line_buf);
                if (!evt) continue;
                cJSON *type = cJSON_GetObjectItem(evt, "type");
                if (!cJSON_IsString(type)) { cJSON_Delete(evt); continue; }
                const char *t = type->valuestring;

                if (strcmp(t, "thinking_delta") == 0) {
                    /* 【优化1】思考链逐字流式（灰色，无换行） */
                    cJSON *d = cJSON_GetObjectItem(evt, "delta");
                    if (cJSON_IsString(d) && d->valuestring) {
                        if (!thinking_active) {
                            uart_puts(COLOR_THINKING);
                            uart_puts(tr("╭ ", "╭ "));
                            thinking_active = 1;
                        }
                        uart_puts(d->valuestring);
                    }
                } else if (strcmp(t, "content") == 0) {
                    if (thinking_active) {
                        uart_puts(COLOR_RESET);
                        uart_puts("\n");
                        thinking_active = 0;
                    }
                    cJSON *delta = cJSON_GetObjectItem(evt, "delta");
                    if (cJSON_IsString(delta) && delta->valuestring) {
                        /* 流式输出：逐行 Markdown 渲染（表格/列表/分隔线/内联） */
                        md_stream_feed(delta->valuestring);
                        /* 累积完整内容 */
                        size_t dl = strlen(delta->valuestring);
                        if (full_len + dl < sizeof(full_buf) - 1) {
                            memcpy(full_buf + full_len, delta->valuestring, dl);
                            full_len += dl;
                            full_buf[full_len] = '\0';
                        }
                    }
                } else if (strcmp(t, "thinking_hide") == 0) {
                    /* 【思考显示 hidden】思考结束后隐藏（清除思考区域） */
                    if (thinking_active) {
                        uart_puts(COLOR_RESET);
                        uart_puts("\r\033[K");   /* 清除当前思考行 */
                        thinking_active = 0;
                    }
                } else if (strcmp(t, "sub_agent") == 0) {
                    /* 【优化2】子 AI 过程事件实时显示 */
                    cJSON *agent = cJSON_GetObjectItem(evt, "agent");
                    cJSON *etype = cJSON_GetObjectItem(evt, "event_type");
                    cJSON *econtent = cJSON_GetObjectItem(evt, "content");
                    const char *a = cJSON_IsString(agent) ? agent->valuestring : "sub_ai";
                    const char *et = cJSON_IsString(etype) ? etype->valuestring : "info";
                    const char *ec = cJSON_IsString(econtent) ? econtent->valuestring : "";
                    uart_puts(COLOR_TOOL);
                    if (strcmp(et, "thinking") == 0)      uart_puts(tr("▸ [", "▸ ["));
                    else if (strcmp(et, "tool_call") == 0) uart_puts(tr("▸ [", "▸ ["));
                    else if (strcmp(et, "tool_result") == 0) uart_puts(tr("✓ [", "✓ ["));
                    else if (strcmp(et, "completed") == 0) uart_puts(tr("✅ [", "✅ ["));
                    else uart_puts(tr("⟳ [", "⟳ ["));
                    uart_puts(a);
                    uart_puts(tr("] ", "] "));
                    uart_puts(COLOR_RESET);
                    uart_puts(ec);
                    uart_puts("\n");
                } else if (strcmp(t, "thinking") == 0) {
                    cJSON *c = cJSON_GetObjectItem(evt, "content");
                    if (cJSON_IsString(c) && c->valuestring) {
                        uart_puts(COLOR_THINKING);
                        uart_puts(tr("╭ Thinking: ", "╭ 思考中："));
                        uart_puts(c->valuestring);
                        uart_puts(COLOR_RESET);
                        uart_puts("\n");
                    }
                } else if (strcmp(t, "tool_call") == 0) {
                    cJSON *name = cJSON_GetObjectItem(evt, "name");
                    cJSON *args = cJSON_GetObjectItem(evt, "args");
                    uart_puts(COLOR_TOOL);
                    uart_puts(tr("▸ ", "▸ "));
                    if (cJSON_IsString(name)) uart_puts(name->valuestring);
                    if (cJSON_IsString(args) && args->valuestring) {
                        uart_puts(" ");
                        uart_puts(args->valuestring);
                    }
                    uart_puts(COLOR_RESET);
                    uart_puts("\n");
                } else if (strcmp(t, "tool_result") == 0) {
                    cJSON *c = cJSON_GetObjectItem(evt, "content");
                    uart_puts(COLOR_GREEN);
                    uart_puts("✓ ");
                    uart_puts(COLOR_RESET);
                    if (cJSON_IsString(c) && c->valuestring) uart_puts(c->valuestring);
                    uart_puts("\n");
                } else if (strcmp(t, "error") == 0) {
                    cJSON *c = cJSON_GetObjectItem(evt, "content");
                    uart_puts(COLOR_RED);
                    if (cJSON_IsString(c) && c->valuestring) uart_puts(c->valuestring);
                    uart_puts(COLOR_RESET);
                    uart_puts("\n");
                } else if (strcmp(t, "done") == 0) {
                    cJSON *c = cJSON_GetObjectItem(evt, "content");
                    if (cJSON_IsString(c) && c->valuestring) {
                        safe_strncpy(full_buf, c->valuestring, sizeof(full_buf));
                    }
                    /* 刷新剩余 Markdown 行缓冲 */
                    md_stream_flush();

                    /* 【状态行】模型 / token 用量 / 缓存命中 / 用时 / tok/s */
                    cJSON *model = cJSON_GetObjectItem(evt, "model");
                    cJSON *usage = cJSON_GetObjectItem(evt, "usage");
                    if (usage && cJSON_IsObject(usage)) {
                        cJSON *pt = cJSON_GetObjectItem(usage, "prompt_tokens");
                        cJSON *ct = cJSON_GetObjectItem(usage, "completion_tokens");
                        cJSON *ch = cJSON_GetObjectItem(usage, "cache_hit");
                        int p = (pt && cJSON_IsNumber(pt)) ? pt->valueint : 0;
                        int c = (ct && cJSON_IsNumber(ct)) ? ct->valueint : 0;
                        int h = (ch && cJSON_IsNumber(ch)) ? ch->valueint : 0;
                        int elapsed = (int)(time(NULL) - req_start);
                        if (elapsed < 1) elapsed = 1;
                        /* tok/s = completion_tokens / elapsed */
                        int tps = (elapsed > 0) ? (c / elapsed) : 0;
                        char sline[256];
                        safe_snprintf(sline, sizeof(sline),
                            "\n%s── %s | ↑%d ↓%d | cache %d | %ds | %d tok/s %s\n",
                            COLOR_DIM,
                            (model && cJSON_IsString(model)) ? model->valuestring : "?",
                            p, c, h, elapsed, tps, COLOR_RESET);
                        uart_puts(sline);
                    }
                    uart_puts("\n");
                    finished = 1;
                }
                cJSON_Delete(evt);
            } else if (line_pos < sizeof(line_buf) - 1) {
                line_pos++;
            }
        }

        /* ---- 授权轮询（每 1 秒检查 auth.sock 待授权请求） ---- */
        if (!auth_pending_active) {
            time_t now = time(NULL);
            if (now - last_auth_check >= 1) {
                last_auth_check = now;
                if (nook_auth_pending(auth_req_id, sizeof(auth_req_id))) {
                    auth_pending_active = 1;
                    uart_puts(COLOR_BOLD COLOR_YELLOW);
                    uart_puts(tr("\n[Authorization] High-risk operation. Enter Y(approve)/N(deny): ",
                                 "\n[授权] 高风险操作，输入 Y(批准)/N(拒绝): "));
                    uart_puts(COLOR_RESET);
                }
            }
        }

        /* ---- 用户 ^N 键 / 授权 Y-N 键 ---- */
        if (pfds[1].revents & POLLIN) {
            char c;
            if (read(STDIN_FILENO, &c, 1) == 1) {
                if (auth_pending_active) {
                    /* 授权响应 */
                    if (c == 'y' || c == 'Y' || c == '\n' || c == '\r') {
                        nook_auth_respond(auth_req_id, 1);
                        uart_puts(tr("✅ Approved.\n", "✅ 已批准。\n"));
                    } else if (c == 'n' || c == 'N') {
                        nook_auth_respond(auth_req_id, 0);
                        uart_puts(tr("❌ Denied.\n", "❌ 已拒绝。\n"));
                    }
                    auth_pending_active = 0;
                    auth_req_id[0] = '\0';
                } else if (c >= '1' && c <= '9') {
                    /* 【优化2】查看第 N 个子 AI 视图（新连接查询，避免与流式冲突） */
                    display_agent_view(c - '1');
                } else if (c == '0') {
                    uart_puts(tr("\n[View] Back to main AI.\n", "\n[视图] 已切回主 AI。\n"));
                }
            }
        }
    }
    close(fd);

    safe_strncpy(resp, full_buf, len);
    resp[len - 1] = '\0';
    LOG_INFO_T("Nook", "AskStream", "Exit", "response length=%zu", strlen(resp));
    return 0;
}
/* ============================================================
 * 新增：带显示控制参数的 nook_ask 变体
 * ============================================================ */
int nook_ask_ollama_with_details(const char *prompt, const char *model,
                                 char *resp, uint32_t len, int timeout_sec,
                                 int show_thinking, int show_tool_calls,
                                 int show_tool_results) {
    LOG_INFO_T("Nook", "AskWithDetails", "Enter", "prompt='%.100s...', show_thinking=%d, show_tool_calls=%d, show_tool_results=%d",
               prompt ? prompt : "(null)", show_thinking, show_tool_calls, show_tool_results);

    /* 直接调用原始 nook_ask_ollama，显示控制参数暂时忽略（功能已在 Python 端实现） */
    /* 如果需要 C 端也支持显示控制，可在此扩展，但当前仅转发 */
    return nook_ask_ollama(prompt, model, resp, len, timeout_sec);
}