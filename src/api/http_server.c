/**
 * @file    http_server.c
 * @brief   HTTP API 服务器（基于 libmicrohttpd）
 * @version 2.0.0.0
 */

/* 【0.5.0 先生要求：链接适配 Ubuntu 22.04~25.10】
 * 默认使用**内置 HTTP 服务器**（src/net/mhd_compat.h）：
 *   Ubuntu 22.04 的 libmicrohttpd12 → libgnutls30 → libidn2-0 → libunistring.so.2
 *   Ubuntu 25.10 的同上链条末环是 libunistring.so.5
 *   → soname 不兼容 → 22.04 编译的二进制在 25.10 上无法启动。
 * 内置实现仅依赖 raw socket + pthread，彻底消除该依赖链。
 * 若确实需要系统 libmicrohttpd：make ENABLE_SYSTEM_MHD=1
 */
#ifdef LINGOS_USE_SYSTEM_MHD
#include <microhttpd.h>
#else
#include "../net/mhd_compat.h"
#endif
#include "log_extra.h"
#include "data_path.h"
#include "safe_string.h"
#include "access_control.h"
#include "../lib/api_log.h"   /* 【0.7.0 P2-B】API 日志 */
#include "nook.h"
#include "system_health.h"
#include "connection_handler.h"
#include "port_config.h"
#include "../ai/ai_server_protocol.h"
#include <pthread.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/time.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <ctype.h>
#include <arpa/inet.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <errno.h>
#include <limits.h>

#define DEFAULT_PORT 8080

static struct MHD_Daemon *mhd_daemon = NULL;
static pthread_t server_thread;
static volatile int running = 0;
static int server_port = DEFAULT_PORT;

/* 【2026-08-22】端口可配（port 指令族）——初始化时从 ports.json 覆盖 */
static void http_server_init_port(void) {
    server_port = port_config_get(PORT_HTTP);
}

static const char WEBUI_HTML[] = "<!DOCTYPE html>\n<html lang=\"zh\">\n<head>\n<meta charset=\"UTF-8\"><meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">\n<title>LING OS Console</title>\n<style>\n:root{--bg:#0D0D10;--surface:#1A1A1F;--red:#E53935;--cyan:#00BCD4;--text:#EEE;--sub:#999}\n*{margin:0;padding:0;box-sizing:border-box}\nbody{background:var(--bg);color:var(--text);font-family:system-ui,sans-serif;height:100vh;display:flex;flex-direction:column}\nheader{padding:12px 20px;border-bottom:1px solid #222;display:flex;align-items:center;gap:10px}\n.dot{width:10px;height:10px;border-radius:50%;background:#E53935}\n.dot.on{background:#4CAF50}\n#chat{flex:1;overflow-y:auto;padding:20px;display:flex;flex-direction:column;gap:12px}\n.msg{max-width:75%;padding:10px 14px;border-radius:14px;line-height:1.5;white-space:pre-wrap}\n.user{align-self:flex-end;background:#E53935;color:#fff}\n.ai{align-self:flex-start;background:var(--surface);color:var(--text)}\n.sys{align-self:center;background:#222;color:var(--sub);font-size:12px;border-radius:8px;padding:4px 10px}\nfooter{padding:12px;border-top:1px solid #222;display:flex;gap:10px}\ninput{flex:1;background:var(--surface);border:1px solid #333;color:var(--text);border-radius:10px;padding:12px;font-size:14px;outline:none}\nbutton{background:var(--red);color:#fff;border:none;border-radius:10px;padding:12px 20px;font-size:14px;cursor:pointer}\nbutton:disabled{opacity:.5}\n</style></head>\n<body>\n<header><div class=\"dot\" id=\"dot\"></div><b>LING OS</b><span id=\"status\" style=\"color:var(--sub);font-size:13px\">checking...</span></header>\n<div id=\"chat\"></div>\n<footer>\n<input id=\"input\" placeholder=\"Type a message...\" autocomplete=\"off\">\n<button id=\"send\">Send</button>\n</footer>\n<script>\nvar chat=document.getElementById('chat'),input=document.getElementById('input'),btn=document.getElementById('send');\nfunction addMsg(t,c){var d=document.createElement('div');d.className='msg '+c;d.textContent=t;chat.appendChild(d);chat.scrollTop=chat.scrollHeight}\nasync function health(){try{var r=await fetch('/system/health');var j=await r.json();document.getElementById('dot').className='dot '+(j.status==='ok'?'on':'');document.getElementById('status').textContent=j.status==='ok'?('healthy | load '+j.load_avg):'error'}catch(e){document.getElementById('status').textContent='offline'}}\nasync function ask(){var p=input.value.trim();if(!p)return;addMsg(p,'user');input.value='';btn.disabled=true;var d=document.createElement('div');d.className='msg ai';d.textContent='...';chat.appendChild(d);chat.scrollTop=chat.scrollHeight;\ntry{var r=await fetch('/nook/ask?prompt='+encodeURIComponent(p));var j=await r.json();d.textContent=j.response||(j.error||'no response')}catch(e){d.textContent='AI service unavailable'}btn.disabled=false;chat.scrollTop=chat.scrollHeight}\nbtn.onclick=ask;input.onkeydown=function(e){if(e.key==='Enter')ask()};addMsg('Connected to LING OS.','sys');health();setInterval(health,10000);\n</script>\n</body></html>";

static int handle_root(struct MHD_Connection *connection) {
    struct MHD_Response *response = MHD_create_response_from_buffer(
        strlen(WEBUI_HTML), (void*)WEBUI_HTML, MHD_RESPMEM_PERSISTENT);
    MHD_add_response_header(response, "Content-Type", "text/html; charset=utf-8");
    MHD_queue_response(connection, MHD_HTTP_OK, response);
    MHD_destroy_response(response);
    return MHD_YES;
}

/* ============================================================
 * 【0.5.0 S11】CORS 头发射回调（供 access_control.c 调用）
 * 放在此文件避免 access_control 直接依赖 microhttpd
 * ============================================================ */
int access_cors_emit_header(void *resp, const char *name, const char *value) {
    if (!resp || !name || !value) return -1;
    return MHD_add_response_header((struct MHD_Response *)resp, name, value);
}

/* ============================================================
 * 【0.5.0 S9】取客户端 IP（用于访问控制分级）
 * MHD_ConnectionInfo 不可用时返回 "0.0.0.0"（按公网处理——最严）
 * ============================================================ */
static const char *http_client_ip(struct MHD_Connection *connection) {
    static __thread char ipbuf[64];
    const union MHD_ConnectionInfo *ci =
        MHD_get_connection_info(connection, MHD_CONNECTION_INFO_CLIENT_ADDRESS);
    if (ci && ci->client_addr) {
        const struct sockaddr *sa = ci->client_addr;
        if (sa->sa_family == AF_INET) {
            const struct sockaddr_in *in4 = (const struct sockaddr_in *)sa;
            struct in_addr a = in4->sin_addr;
            safe_snprintf(ipbuf, sizeof(ipbuf), "%s", inet_ntoa(a));
            return ipbuf;
        } else if (sa->sa_family == AF_INET6) {
            const struct sockaddr_in6 *in6 = (const struct sockaddr_in6 *)sa;
            /* IPv4 映射 → 还原为 IPv4 */
            if (IN6_IS_ADDR_V4MAPPED(&in6->sin6_addr)) {
                unsigned char b[4];
                memcpy(b, in6->sin6_addr.s6_addr + 12, 4);
                safe_snprintf(ipbuf, sizeof(ipbuf), "%u.%u.%u.%u", b[0], b[1], b[2], b[3]);
                return ipbuf;
            }
            char tmp[64] = {0};
            if (inet_ntop(AF_INET6, &in6->sin6_addr, tmp, sizeof(tmp)))
                safe_snprintf(ipbuf, sizeof(ipbuf), "%s", tmp);
            else
                safe_snprintf(ipbuf, sizeof(ipbuf), "::1");
            return ipbuf;
        }
    }
    safe_snprintf(ipbuf, sizeof(ipbuf), "0.0.0.0");   /* 未知 → 最严 */
    return ipbuf;
}

static void send_json_response(struct MHD_Connection *connection, int status_code, const char *json) {
    /* 【0.7.0 P2-B】API 日志（响应侧——server mode 可查看） */
    api_log("http", "out", "-", status_code, 0,
            json ? (long)strlen(json) : 0, NULL);
    struct MHD_Response *response = MHD_create_response_from_buffer(strlen(json), (void*)json, MHD_RESPMEM_PERSISTENT);
    MHD_add_response_header(response, "Content-Type", "application/json");
    MHD_queue_response(connection, status_code, response);
    MHD_destroy_response(response);
}

/* ============================================================
 * 【协议v3】文件端点（HTTP 8080 独立通道——Bearer token 认证）
 * ============================================================ */
#define MAX_FILE_PATH 512
#define MAX_FILE_JSON 65536

static int http_auth_check(struct MHD_Connection *connection) {
    const char *auth = MHD_lookup_connection_value(connection, MHD_HEADER_KIND, "Authorization");
    if (!auth || strncmp(auth, "Bearer ", 7) != 0) return 0;
    const char *token = auth + 7;
    return connection_verify_token(token);
}

static int handle_files_list(struct MHD_Connection *connection, const char *path) {
    if (!path || path[0] == '\0') path = "/";
    DIR *d = opendir(path);
    if (!d) {
        char buf[256];
        safe_snprintf(buf, sizeof(buf), "{\"error\":\"cannot open dir\",\"path\":\"%s\"}", path);
        send_json_response(connection, MHD_HTTP_NOT_FOUND, buf);
        return MHD_YES;
    }
    char *json = malloc(MAX_FILE_JSON);
    if (!json) { closedir(d); send_json_response(connection, MHD_HTTP_INTERNAL_SERVER_ERROR, "{\"error\":\"alloc\"}"); return MHD_YES; }
    size_t pos = 0;
    pos += (size_t)snprintf(json + pos, MAX_FILE_JSON - pos, "{\"path\":\"%s\",\"entries\":[", path);
    struct dirent *e;
    while ((e = readdir(d)) != NULL && pos < MAX_FILE_JSON - 256) {
        if (e->d_name[0] == '.') continue;
        char full[MAX_FILE_PATH];
        safe_snprintf(full, sizeof(full), "%s/%s", path, e->d_name);
        struct stat st;
        int is_dir = 0;
        long size = 0;
        if (stat(full, &st) == 0) {
            is_dir = S_ISDIR(st.st_mode);
            size = (long)st.st_size;
        }
        pos += (size_t)snprintf(json + pos, MAX_FILE_JSON - pos,
                "%s{\"name\":\"%s\",\"type\":\"%s\",\"size\":%ld}",
                pos > (strlen("{\"path\":\"\",\"entries\":[") - 1) ? "," : "",
                e->d_name, is_dir ? "dir" : "file", size);
    }
    closedir(d);
    if (pos >= MAX_FILE_JSON - 16) pos = MAX_FILE_JSON - 16;
    pos += (size_t)snprintf(json + pos, MAX_FILE_JSON - pos, "]}");
    struct MHD_Response *resp = MHD_create_response_from_buffer(pos, json, MHD_RESPMEM_MUST_FREE);
    MHD_add_response_header(resp, "Content-Type", "application/json");
    MHD_queue_response(connection, MHD_HTTP_OK, resp);
    MHD_destroy_response(resp);
    return MHD_YES;
}

static int handle_files_download(struct MHD_Connection *connection, const char *path) {
    if (!path || path[0] == '\0') {
        send_json_response(connection, MHD_HTTP_BAD_REQUEST, "{\"error\":\"missing path\"}");
        return MHD_YES;
    }
    FILE *fp = fopen(path, "rb");
    if (!fp) {
        send_json_response(connection, MHD_HTTP_NOT_FOUND, "{\"error\":\"file not found\"}");
        return MHD_YES;
    }
    fseek(fp, 0, SEEK_END);
    long sz = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    if (sz < 0 || sz > 512L * 1024 * 1024) { fclose(fp); send_json_response(connection, MHD_HTTP_FORBIDDEN, "{\"error\":\"file too large\"}"); return MHD_YES; }
    char *data = malloc((size_t)sz + 1);
    if (!data) { fclose(fp); send_json_response(connection, MHD_HTTP_INTERNAL_SERVER_ERROR, "{\"error\":\"alloc\"}"); return MHD_YES; }
    size_t rd = fread(data, 1, (size_t)sz, fp);
    fclose(fp);
    struct MHD_Response *resp = MHD_create_response_from_buffer(rd, data, MHD_RESPMEM_MUST_FREE);
    MHD_add_response_header(resp, "Content-Type", "application/octet-stream");
    MHD_queue_response(connection, MHD_HTTP_OK, resp);
    MHD_destroy_response(resp);
    return MHD_YES;
}

static int handle_files_delete(struct MHD_Connection *connection, const char *path) {
    if (!path || path[0] == '\0') {
        send_json_response(connection, MHD_HTTP_BAD_REQUEST, "{\"error\":\"missing path\"}");
        return MHD_YES;
    }
    if (unlink(path) == 0) {
        send_json_response(connection, MHD_HTTP_OK, "{\"status\":\"ok\"}");
    } else {
        char buf[256];
        safe_snprintf(buf, sizeof(buf), "{\"error\":\"delete failed\",\"errno\":%d}", errno);
        send_json_response(connection, MHD_HTTP_INTERNAL_SERVER_ERROR, buf);
    }
    return MHD_YES;
}

/* ---- upload/命令状态机（MHD 分块回调） ---- */
struct upload_ctx {
    FILE *fp;
    char path[MAX_FILE_PATH];
    int failed;
    int mode;              /* 0=文件上传 1=/api/cmd 命令代理 2=/api/webhook 外部触发 */
    char *buf;             /* mode>=1: 累积请求体 */
    size_t buf_len;
    size_t buf_cap;
    char webhook_id[64];   /* mode=2: webhook ID（URL 尾段，已过滤字符） */
};

static void free_upload_ctx(struct upload_ctx *ctx) {
    if (!ctx) return;
    if (ctx->fp) fclose(ctx->fp);
    if (ctx->buf) free(ctx->buf);
    free(ctx);
}

/* ============================================================
 * 【0.4.3】POST /api/cmd —— 网页/任意 HTTP 客户端统一命令代理
 * body: {"cmd":"session_list","params":{...}} → ai.sock → JSON 响应
 * （网页 UI 借此读取全部真实数据——与 WS/App 命令同源）
 * ============================================================ */
static void api_cmd_forward(struct MHD_Connection *connection, const char *body) {
    if (!body || !*body) {
        send_json_response(connection, MHD_HTTP_BAD_REQUEST,
                           "{\"status\":\"error\",\"msg\":\"empty body\"}");
        return;
    }
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        send_json_response(connection, MHD_HTTP_INTERNAL_SERVER_ERROR,
                           "{\"status\":\"error\",\"msg\":\"socket error\"}");
        return;
    }
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    safe_strncpy(addr.sun_path, AI_SOCKET_PATH, sizeof(addr.sun_path));
    struct timeval tv;
    tv.tv_sec = 15;
    tv.tv_usec = 0;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        close(fd);
        send_json_response(connection, MHD_HTTP_SERVICE_UNAVAILABLE,
                           "{\"status\":\"error\",\"msg\":\"ai server unavailable\"}");
        return;
    }
    char req[8192];
    safe_snprintf(req, sizeof(req), "%s\n", body);
    ssize_t n = write(fd, req, strlen(req));
    if (n <= 0) {
        close(fd);
        send_json_response(connection, MHD_HTTP_INTERNAL_SERVER_ERROR,
                           "{\"status\":\"error\",\"msg\":\"write failed\"}");
        return;
    }
    char buf[32768];
    ssize_t r = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (r <= 0) {
        send_json_response(connection, MHD_HTTP_GATEWAY_TIMEOUT,
                           "{\"status\":\"error\",\"msg\":\"no response\"}");
        return;
    }
    buf[r] = '\0';
    if (r > 0 && buf[r - 1] == '\n') buf[--r] = '\0';
    /* 透传 ai_server 的 JSON（含 cmd 标识） */
    struct MHD_Response *resp = MHD_create_response_from_buffer(
        (size_t)r, buf, MHD_RESPMEM_MUST_COPY);
    MHD_add_response_header(resp, "Content-Type", "application/json");
    /* 【0.5.0 S11】CORS 不再用 `*` —— 校验 Origin（允许同源/本机；他人站点被浏览器拦截） */
    {
        const char *origin = MHD_lookup_connection_value(connection, MHD_HEADER_KIND, "Origin");
        const char *hosthdr = MHD_lookup_connection_value(connection, MHD_HEADER_KIND, "Host");
        access_cors_add_headers(resp, origin, hosthdr);
    }
    MHD_queue_response(connection, MHD_HTTP_OK, resp);
    MHD_destroy_response(resp);
}

/* ============================================================
 * 【0.4.3】网页 UI 静态页（share/webui/index.html——可热更新）
 * 文件不存在时回退内嵌 WEBUI_HTML（防弹）
 * ============================================================ */
static int handle_ui_page(struct MHD_Connection *connection) {
    const char *root = lingos_data_root();
    char path[512];
    safe_snprintf(path, sizeof(path), "%s/share/webui/index.html", root);
    FILE *fp = fopen(path, "rb");
    if (!fp) {
        return handle_root(connection);
    }
    fseek(fp, 0, SEEK_END);
    long sz = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    if (sz <= 0 || sz > 4 * 1024 * 1024) {
        fclose(fp);
        return handle_root(connection);
    }
    char *data = (char *)malloc((size_t)sz);
    if (!data) {
        fclose(fp);
        return handle_root(connection);
    }
    size_t rd = fread(data, 1, (size_t)sz, fp);
    fclose(fp);
    if (rd != (size_t)sz) {
        free(data);
        return handle_root(connection);
    }
    struct MHD_Response *resp = MHD_create_response_from_buffer(
        (size_t)sz, data, MHD_RESPMEM_MUST_COPY);
    MHD_add_response_header(resp, "Content-Type", "text/html; charset=utf-8");
    /* 【0.5.0 S11】静态页同样校验 Origin（原为 `*`） */
    {
        const char *origin = MHD_lookup_connection_value(connection, MHD_HEADER_KIND, "Origin");
        const char *hosthdr = MHD_lookup_connection_value(connection, MHD_HEADER_KIND, "Host");
        access_cors_add_headers(resp, origin, hosthdr);
    }
    MHD_queue_response(connection, MHD_HTTP_OK, resp);
    MHD_destroy_response(resp);
    free(data);
    return MHD_YES;
}

static enum MHD_Result upload_handler(struct MHD_Connection *connection,
                                      const char *url, const char *method,
                                      const char *upload_data, size_t *upload_data_size,
                                      void **con_cls) {
    (void)url;
    struct upload_ctx *ctx = *con_cls;
    if (!ctx) {
        ctx = calloc(1, sizeof(struct upload_ctx));
        if (!ctx) return MHD_NO;
        const char *path = MHD_lookup_connection_value(connection, MHD_GET_ARGUMENT_KIND, "path");
        if (path && path[0]) {
            safe_strncpy(ctx->path, path, sizeof(ctx->path));
            ctx->fp = fopen(ctx->path, "wb");
            if (!ctx->fp) ctx->failed = 1;
        } else {
            ctx->failed = 1;
        }
        *con_cls = ctx;
        return MHD_YES;
    }
    if (*upload_data_size != 0) {
        if (ctx->mode >= 1) {
            /* 命令代理 / webhook：累积 body */
            size_t need = ctx->buf_len + *upload_data_size + 1;
            if (need > ctx->buf_cap) {
                size_t ncap = ctx->buf_cap ? ctx->buf_cap * 2 : 1024;
                while (ncap < need) ncap *= 2;
                char *nb = realloc(ctx->buf, ncap);
                if (!nb) {
                    ctx->failed = 1;
                    *upload_data_size = 0;
                    return MHD_YES;
                }
                ctx->buf = nb;
                ctx->buf_cap = ncap;
            }
            memcpy(ctx->buf + ctx->buf_len, upload_data, *upload_data_size);
            ctx->buf_len += *upload_data_size;
            ctx->buf[ctx->buf_len] = '\0';
            *upload_data_size = 0;
            return MHD_YES;
        }
        if (ctx->fp && !ctx->failed) {
            size_t w = fwrite(upload_data, 1, *upload_data_size, ctx->fp);
            if (w != *upload_data_size) ctx->failed = 1;
        }
        *upload_data_size = 0;
        return MHD_YES;
    }
    if (strcmp(method, "POST") == 0) {
        if (ctx->mode == 1) {
            /* 完成：转发 ai.sock 返回 JSON */
            if (ctx->failed || !ctx->buf || !ctx->buf_len) {
                send_json_response(connection, MHD_HTTP_BAD_REQUEST,
                                   "{\"status\":\"error\",\"msg\":\"empty body\"}");
            } else {
                api_cmd_forward(connection, ctx->buf);
            }
            free_upload_ctx(ctx);
            *con_cls = NULL;
            return MHD_YES;
        }
        if (ctx->mode == 2) {
            /* 【2026-09-18】Webhook 外部触发：组装 webhook_trigger 命令 → ai.sock
             * body 为 JSON（{ 或 [ 开头）时内联 payload（≤4KB）；否则仅触发 */
            int is_json = (ctx->buf && ctx->buf_len && ctx->buf[0] &&
                           (ctx->buf[0] == '{' || ctx->buf[0] == '['));
            char reqbuf[8192];
            if (is_json && ctx->buf_len <= 4096) {
                safe_snprintf(reqbuf, sizeof(reqbuf),
                              "{\"cmd\":\"webhook_trigger\",\"webhook_id\":\"%s\",\"payload\":%s}",
                              ctx->webhook_id, ctx->buf);
            } else {
                safe_snprintf(reqbuf, sizeof(reqbuf),
                              "{\"cmd\":\"webhook_trigger\",\"webhook_id\":\"%s\"}",
                              ctx->webhook_id);
            }
            api_cmd_forward(connection, reqbuf);
            free_upload_ctx(ctx);
            *con_cls = NULL;
            return MHD_YES;
        }
        int status = (ctx->fp && !ctx->failed) ? MHD_HTTP_OK : MHD_HTTP_INTERNAL_SERVER_ERROR;
        send_json_response(connection, status, (ctx->fp && !ctx->failed)
                           ? "{\"status\":\"ok\"}" : "{\"error\":\"upload failed\"}");
    }
    free_upload_ctx(ctx);
    *con_cls = NULL;
    return MHD_YES;
}



static int handle_health(struct MHD_Connection *connection) {
    char buf[1024];
    int mem = get_memory_usage();
    const char *root = lingos_data_root();
    int disk = get_disk_usage(root);
    double load1, load5, load15;
    get_load_avg(&load1, &load5, &load15);
    snprintf(buf, sizeof(buf),
        "{\"status\":\"ok\",\"memory_usage\":%d,\"disk_usage\":%d,\"load_avg\":%.2f,\"python\":%d,\"ai\":%d,\"network\":%d}",
        mem, disk, load1, check_python(), check_ai_backend(), check_network());
    send_json_response(connection, MHD_HTTP_OK, buf);
    return MHD_YES;
}

static int handle_nook_ask(struct MHD_Connection *connection, const char *prompt) {
    if (!prompt || !*prompt) {
        send_json_response(connection, MHD_HTTP_BAD_REQUEST, "{\"error\":\"missing prompt\"}");
        return MHD_YES;
    }
    char response[4096];
    int ret = nook_ask_ollama(prompt, NULL, response, sizeof(response), 30);
    if (ret == 0) {
        char json[8192];
        snprintf(json, sizeof(json), "{\"status\":\"ok\",\"response\":\"%s\"}", response);
        send_json_response(connection, MHD_HTTP_OK, json);
    } else {
        send_json_response(connection, MHD_HTTP_INTERNAL_SERVER_ERROR, "{\"error\":\"AI service unavailable\"}");
    }
    return MHD_YES;
}

static enum MHD_Result request_handler(void *cls,
                                       struct MHD_Connection *connection,
                                       const char *url,
                                       const char *method,
                                       const char *version,
                                       const char *upload_data,
                                       size_t *upload_data_size,
                                       void **con_cls) {
    (void)cls; (void)version;

    /* 【0.7.0 P2-B】API 日志（请求侧——server mode 可查看） */
    api_log("http", "in", url ? url : "-", 0, 0, 0, method ? method : NULL);

    /* 【协议v3】POST 上传分块回调（upload_data 累积） */
    if (*con_cls != NULL) {
        return upload_handler(connection, url, method, upload_data, upload_data_size, con_cls);
    }

    /* 【0.4.3】POST /api/cmd —— 命令代理（网页 UI 数据源） */
    if (strcmp(url, "/api/cmd") == 0 && strcmp(method, "POST") == 0) {
        /*
         * 【0.5.0 S9/S18】访问控制（先生裁决）
         *   · localhost          → 免 token
         *   · 局域网 + lan_no_token(默认开) → 免 token
         *   · 公网                → 必须 Bearer token
         *   · 限流：默认开启；局域网可配《允许局域网内连接不限流》
         */
        const char *cip = http_client_ip(connection);

        /* 限流（先生 S18） */
        if (!access_rate_allow(cip)) {
            send_json_response(connection, MHD_HTTP_TOO_MANY_REQUESTS,
                               "{\"status\":\"error\",\"code\":\"rate_limited\","
                               "\"msg\":\"请求过于频繁（可开启「允许局域网内连接不限流」）\"}");
            return MHD_YES;
        }

        /* 认证（先生 S9） */
        {
            const char *auth = MHD_lookup_connection_value(connection, MHD_HEADER_KIND, "Authorization");
            int has_token = (auth && strncmp(auth, "Bearer ", 7) == 0);
            if (access_needs_token(cip, has_token)) {
                if (!has_token) {
                    send_json_response(connection, MHD_HTTP_UNAUTHORIZED,
                                       "{\"status\":\"error\",\"code\":\"unauthorized\","
                                       "\"msg\":\"公网访问需要 Bearer token\"}");
                    return MHD_YES;
                }
                /* 有 token → 必须有效 */
                if (!connection_verify_token(auth + 7)) {
                    send_json_response(connection, MHD_HTTP_UNAUTHORIZED,
                                       "{\"status\":\"error\",\"code\":\"invalid_token\","
                                       "\"msg\":\"token 无效或已过期\"}");
                    return MHD_YES;
                }
            }
        }

        struct upload_ctx *ctx = calloc(1, sizeof(struct upload_ctx));
        if (!ctx) return MHD_NO;
        ctx->mode = 1;
        *con_cls = ctx;
        return MHD_YES;
    }

    /* 【2026-09-18 接线】POST /api/webhook/<id> —— 外部系统触发（此前只有注册表无路由）
     * 流程：URL 提取 id → 累积 body → 转发 ai.sock（webhook_trigger → home_ext 执行绑定动作）
     * 安全：id 字符过滤（防注入）+ 限流 + 公网需 Bearer token（局域网按 S9 策略免 token） */
    if (strncmp(url, "/api/webhook/", 13) == 0 && strcmp(method, "POST") == 0) {
        const char *wid = url + 13;
        char safe_id[64];
        size_t wi = 0;
        for (const char *p = wid; *p && wi < 48; p++) {
            if (isalnum((unsigned char)*p) || *p == '_' || *p == '-') {
                safe_id[wi++] = *p;
            } else {
                break;   /* 遇到非法字符（含 / ? 等）即止 */
            }
        }
        safe_id[wi] = '\0';
        if (wi == 0) {
            send_json_response(connection, MHD_HTTP_BAD_REQUEST,
                               "{\"status\":\"error\",\"msg\":\"missing webhook id\"}");
            return MHD_YES;
        }

        const char *cip = http_client_ip(connection);
        if (!access_rate_allow(cip)) {
            send_json_response(connection, MHD_HTTP_TOO_MANY_REQUESTS,
                               "{\"status\":\"error\",\"code\":\"rate_limited\"}");
            return MHD_YES;
        }
        {
            const char *auth = MHD_lookup_connection_value(connection, MHD_HEADER_KIND, "Authorization");
            int has_token = (auth && strncmp(auth, "Bearer ", 7) == 0);
            if (access_needs_token(cip, has_token)) {
                if (!has_token || !connection_verify_token(auth + 7)) {
                    send_json_response(connection, MHD_HTTP_UNAUTHORIZED,
                                       "{\"status\":\"error\",\"code\":\"unauthorized\","
                                       "\"msg\":\"公网 webhook 需要有效 Bearer token\"}");
                    return MHD_YES;
                }
            }
        }

        struct upload_ctx *ctx = calloc(1, sizeof(struct upload_ctx));
        if (!ctx) return MHD_NO;
        ctx->mode = 2;
        safe_strncpy(ctx->webhook_id, safe_id, sizeof(ctx->webhook_id));
        *con_cls = ctx;
        return MHD_YES;
    }

    /* 文件端点：Bearer token 认证 */
    if (strncmp(url, "/api/files", 10) == 0) {
        /* 【0.7.0 S18】补限流（此前仅 /api/cmd 与 webhook 有限流） */
        if (!access_rate_allow(http_client_ip(connection))) {
            send_json_response(connection, MHD_HTTP_TOO_MANY_REQUESTS,
                               "{\"status\":\"error\",\"code\":\"rate_limited\"}");
            return MHD_YES;
        }
        if (!http_auth_check(connection)) {
            send_json_response(connection, MHD_HTTP_UNAUTHORIZED, "{\"error\":\"unauthorized\"}");
            return MHD_YES;
        }
        const char *path = MHD_lookup_connection_value(connection, MHD_GET_ARGUMENT_KIND, "path");
        if (strcmp(url, "/api/files/list") == 0 && strcmp(method, "GET") == 0) {
            return handle_files_list(connection, path);
        }
        if (strcmp(url, "/api/files/download") == 0 && strcmp(method, "GET") == 0) {
            return handle_files_download(connection, path);
        }
        if (strcmp(url, "/api/files") == 0 && strcmp(method, "DELETE") == 0) {
            return handle_files_delete(connection, path);
        }
        if (strcmp(url, "/api/files/upload") == 0 && strcmp(method, "POST") == 0) {
            /* 首包：创建上传上下文 */
            struct upload_ctx *ctx = calloc(1, sizeof(struct upload_ctx));
            if (!ctx) return MHD_NO;
            if (path && path[0]) {
                safe_strncpy(ctx->path, path, sizeof(ctx->path));
                ctx->fp = fopen(ctx->path, "wb");
                if (!ctx->fp) ctx->failed = 1;
            } else {
                ctx->failed = 1;
            }
            *con_cls = ctx;
            return MHD_YES;
        }
        send_json_response(connection, MHD_HTTP_METHOD_NOT_ALLOWED, "{\"error\":\"method not allowed\"}");
        return MHD_YES;
    }

    if (strcmp(method, "GET") != 0) {
        send_json_response(connection, MHD_HTTP_METHOD_NOT_ALLOWED, "{\"error\":\"method not allowed\"}");
        return MHD_YES;
    }
    if (strcmp(url, "/") == 0 || strcmp(url, "/console") == 0 || strcmp(url, "/ui") == 0) {
        return handle_ui_page(connection);
    } else if (strcmp(url, "/system/health") == 0) {
        return handle_health(connection);
    } else if (strcmp(url, "/nook/ask") == 0) {
        /* 【0.7.0 S18】补限流 */
        if (!access_rate_allow(http_client_ip(connection))) {
            send_json_response(connection, MHD_HTTP_TOO_MANY_REQUESTS,
                               "{\"status\":\"error\",\"code\":\"rate_limited\"}");
            return MHD_YES;
        }
        const char *prompt = MHD_lookup_connection_value(connection, MHD_GET_ARGUMENT_KIND, "prompt");
        return handle_nook_ask(connection, prompt);
    } else {
        send_json_response(connection, MHD_HTTP_NOT_FOUND, "{\"error\":\"endpoint not found\"}");
        return MHD_YES;
    }
}

static void* server_loop(void *arg) {
    (void)arg;
    mhd_daemon = MHD_start_daemon(MHD_USE_AUTO | MHD_USE_INTERNAL_POLLING_THREAD,
                                   server_port, NULL, NULL,
                                   &request_handler, NULL,
                                   MHD_OPTION_END);
    if (!mhd_daemon) {
        LOG_ERROR_T("HTTPServer", "Start", "Fail", "MHD_start_daemon failed");
        running = 0;
        return NULL;
    }
    LOG_INFO_T("HTTPServer", "Start", "OK", "listening on port %d", server_port);
    running = 1;
    while (running) sleep(1);
    MHD_stop_daemon(mhd_daemon);
    mhd_daemon = NULL;
    return NULL;
}

int http_server_start(int port) {
    if (running) return 0;
    /* 【2026-08-22】端口可配：port<=0 时用 ports.json 配置值 */
    server_port = (port > 0) ? port : port_config_get(PORT_HTTP);
    int ret = pthread_create(&server_thread, NULL, server_loop, NULL);
    if (ret != 0) {
        LOG_ERROR_T("HTTPServer", "Start", "ThreadFail", "pthread_create error %d", ret);
        return -1;
    }
    return 0;
}

void http_server_stop(void) {
    if (!running) return;
    running = 0;
    pthread_join(server_thread, NULL);
}

int http_server_is_running(void) {
    return running;
}