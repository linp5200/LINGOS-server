/**
 * @file    http_server.c
 * @brief   HTTP API 服务器（基于 libmicrohttpd）
 * @version 2.0.0.0
 */

#include <microhttpd.h>
#include "log_extra.h"
#include "data_path.h"
#include "safe_string.h"
#include "nook.h"
#include "system_health.h"
#include "connection_handler.h"
#include "port_config.h"
#include <pthread.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/stat.h>
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

static void send_json_response(struct MHD_Connection *connection, int status_code, const char *json) {
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

/* ---- upload 状态机（MHD 分块回调） ---- */
struct upload_ctx {
    FILE *fp;
    char path[MAX_FILE_PATH];
    int failed;
};

static void free_upload_ctx(struct upload_ctx *ctx) {
    if (!ctx) return;
    if (ctx->fp) fclose(ctx->fp);
    free(ctx);
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
        if (ctx->fp && !ctx->failed) {
            size_t w = fwrite(upload_data, 1, *upload_data_size, ctx->fp);
            if (w != *upload_data_size) ctx->failed = 1;
        }
        *upload_data_size = 0;
        return MHD_YES;
    }
    if (strcmp(method, "POST") == 0) {
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

    /* 【协议v3】POST 上传分块回调（upload_data 累积） */
    if (*con_cls != NULL) {
        return upload_handler(connection, url, method, upload_data, upload_data_size, con_cls);
    }

    /* 文件端点：Bearer token 认证 */
    if (strncmp(url, "/api/files", 10) == 0) {
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
    if (strcmp(url, "/") == 0 || strcmp(url, "/console") == 0) {
        return handle_root(connection);
    } else if (strcmp(url, "/system/health") == 0) {
        return handle_health(connection);
    } else if (strcmp(url, "/nook/ask") == 0) {
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