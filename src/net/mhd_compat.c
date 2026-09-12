/**
 * @file    src/net/mhd_compat.c
 * @brief   内置 HTTP 服务器实现（libmicrohttpd 兼容层）
 * @version LN-0.5.0
 *
 * 实现要点：
 *   · 监听 socket + accept 线程 + 每连接线程（线程分离，无连接数上限硬限制）
 *   · HTTP/1.1 请求解析：请求行 / 头 / 查询串 / Content-Length body
 *   · 响应：状态行 + 头 + body，`Connection: close` 短连接
 *   · 语义对齐 MHD 的三段式上传回调（§MHD_OPTION 说明见头文件）
 *
 * 安全：
 *   · 请求头上限 64KB（防内存耗尽，OWASP LLM10）
 *   · body 上限 64MB（防 DoS）
 *   · 逐连接超时（SO_RCVTIMEO/SO_SNDTIMEO）
 */

#include "mhd_compat.h"
#include "../lib/log_extra.h"
#include "../common/safe_string.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <errno.h>
#include <ctype.h>
#include <time.h>
#include <pthread.h>
#include <arpa/inet.h>
#include <netinet/tcp.h>
#include <netdb.h>

/* ============================================================
 * 限额
 * ============================================================ */
#define MHD_REQ_HEAD_MAX   (64 * 1024)          /* 请求头（含请求行）上限 */
#define MHD_BODY_MAX       (64u * 1024 * 1024)  /* body 上限 */
#define MHD_CONN_TIMEOUT   30                    /* 单连接秒 */
#define MHD_LISTEN_BACKLOG 128

/* ============================================================
 * 键值对
 * ============================================================ */
typedef struct {
    char *key;
    char *val;
} mhd_kv_t;

/* ============================================================
 * 响应对象
 * ============================================================ */
struct MHD_Response {
    char   *body;
    size_t  body_len;
    int     must_free;          /* 1 = 销毁时 free(body) */
    mhd_kv_t *headers;
    int     header_count;
    int     header_cap;
};

/* ============================================================
 * 连接对象
 * ============================================================ */
struct MHD_Connection {
    int fd;
    struct sockaddr_storage addr;
    socklen_t addr_len;

    char  *method;
    char  *url_path;            /* 不含 query */
    char  *url_full;            /* 原样（含 query） */
    char  *version;

    mhd_kv_t *headers;
    int       header_count;
    mhd_kv_t *args;             /* GET 参数 */
    int       arg_count;

    MHD_Response **queue;
    int       queue_count;
    int       queue_cap;
};

/* ============================================================
 * Daemon
 * ============================================================ */
struct MHD_Daemon {
    int      listen_fd;
    unsigned short port;
    MHD_RequestCallback rcb;
    void    *rcb_cls;
    volatile int running;
    pthread_t accept_thread;
};

/* ============================================================
 * 小工具
 * ============================================================ */
static char *xstrdup(const char *s) {
    if (!s) return NULL;
    size_t n = strlen(s);
    char *p = malloc(n + 1);
    if (!p) return NULL;
    memcpy(p, s, n + 1);
    return p;
}

/* URL 解码（%XX 与 +） */
static void url_decode(char *s) {
    if (!s) return;
    char *w = s;
    for (char *r = s; *r; r++) {
        if (*r == '%' && isxdigit((unsigned char)r[1]) && isxdigit((unsigned char)r[2])) {
            char hex[3] = { r[1], r[2], 0 };
            *w++ = (char)strtol(hex, NULL, 16);
            r += 2;
        } else if (*r == '+') {
            *w++ = ' ';
        } else {
            *w++ = *r;
        }
    }
    *w = '\0';
}

static int kv_add(mhd_kv_t **arr, int *count, int *cap, const char *k, const char *v) {
    if (*count >= *cap) {
        int ncap = *cap ? *cap * 2 : 16;
        mhd_kv_t *na = realloc(*arr, (size_t)ncap * sizeof(mhd_kv_t));
        if (!na) return -1;
        *arr = na;
        *cap = ncap;
    }
    (*arr)[*count].key = xstrdup(k ? k : "");
    (*arr)[*count].val = xstrdup(v ? v : "");
    if (!(*arr)[*count].key || !(*arr)[*count].val) {
        free((*arr)[*count].key);
        free((*arr)[*count].val);
        return -1;
    }
    (*count)++;
    return 0;
}

static void kv_free(mhd_kv_t *arr, int count) {
    if (!arr) return;
    for (int i = 0; i < count; i++) {
        free(arr[i].key);
        free(arr[i].val);
    }
    free(arr);
}

static const char *kv_get(mhd_kv_t *arr, int count, const char *key) {
    if (!arr || !key) return NULL;
    for (int i = 0; i < count; i++) {
        if (arr[i].key && strcasecmp(arr[i].key, key) == 0) return arr[i].val;
    }
    return NULL;
}

/* 状态码 → 原因短语 */
static const char *reason_phrase(unsigned int code) {
    switch (code) {
        case 200: return "OK";
        case 204: return "No Content";
        case 301: return "Moved Permanently";
        case 302: return "Found";
        case 304: return "Not Modified";
        case 400: return "Bad Request";
        case 401: return "Unauthorized";
        case 403: return "Forbidden";
        case 404: return "Not Found";
        case 405: return "Method Not Allowed";
        case 413: return "Payload Too Large";
        case 429: return "Too Many Requests";
        case 500: return "Internal Server Error";
        case 501: return "Not Implemented";
        case 503: return "Service Unavailable";
        case 504: return "Gateway Timeout";
        default:  return "OK";
    }
}

/* ============================================================
 * 请求解析
 * ============================================================ */
/* 读取到 \r\n\r\n（或 \n\n）为止；返回头长度，-1 失败 */
static ssize_t read_request_head(int fd, char *buf, size_t cap) {
    size_t n = 0;
    while (n + 1 < cap) {
        ssize_t r = recv(fd, buf + n, cap - 1 - n, 0);
        if (r <= 0) return (n > 0) ? (ssize_t)n : -1;
        n += (size_t)r;
        buf[n] = '\0';
        if (strstr(buf, "\r\n\r\n")) return (ssize_t)n;
        if (strstr(buf, "\n\n"))     return (ssize_t)n;
    }
    return -1;
}

static int parse_request(MHD_Connection *c, char *head) {
    /* ① 请求行 */
    char *line_end = strstr(head, "\r\n");
    size_t nl = 2;
    if (!line_end) { line_end = strstr(head, "\n"); nl = 1; }
    if (!line_end) return -1;
    *line_end = '\0';

    char *sp1 = strchr(head, ' ');
    if (!sp1) return -1;
    *sp1 = '\0';
    char *sp2 = strchr(sp1 + 1, ' ');
    if (!sp2) return -1;
    *sp2 = '\0';
    c->method  = xstrdup(head);
    c->url_full = xstrdup(sp1 + 1);
    c->version = xstrdup(sp2 + 1);
    if (!c->method || !c->url_full || !c->version) return -1;

    /* 拆 query */
    char *q = strchr(c->url_full, '?');
    if (q) {
        *q = '\0';
        c->url_path = xstrdup(c->url_full);
        *q = '?';    /* 恢复原样（保持 url_full 完整） */
        /* 解析 query 参数 */
        char *qs = xstrdup(q + 1);
        if (qs) {
            int acap = 0;
            char *save = NULL;
            for (char *tok = strtok_r(qs, "&", &save); tok; tok = strtok_r(NULL, "&", &save)) {
                char *eq = strchr(tok, '=');
                if (eq) { *eq = '\0'; url_decode(tok); url_decode(eq + 1); kv_add(&c->args, &c->arg_count, &acap, tok, eq + 1); }
                else    { url_decode(tok); kv_add(&c->args, &c->arg_count, &acap, tok, ""); }
            }
            free(qs);
        }
    } else {
        c->url_path = xstrdup(c->url_full);
    }
    if (!c->url_path) return -1;

    /* ② 头 */
    char *p = line_end + nl;
    int hcap = 0;
    while (p && *p) {
        char *e = strstr(p, "\r\n");
        size_t step = 2;
        if (!e) { e = strstr(p, "\n"); step = 1; }
        if (!e) break;
        *e = '\0';
        if (e == p) break;                 /* 空行 = 头结束 */
        char *colon = strchr(p, ':');
        if (colon) {
            *colon = '\0';
            char *v = colon + 1;
            while (*v == ' ' || *v == '\t') v++;
            kv_add(&c->headers, &c->header_count, &hcap, p, v);
        }
        p = e + step;
    }
    return 0;
}

/* ============================================================
 * 响应发送
 * ============================================================ */
static int response_send(MHD_Connection *c, MHD_Response *r, unsigned int status) {
    char head[4096];
    int used = safe_snprintf(head, sizeof(head),
                             "HTTP/1.1 %u %s\r\n", status, reason_phrase(status));
    if (used <= 0) return -1;

    int has_ct = 0, has_cl = 0;
    for (int i = 0; i < r->header_count; i++) {
        if (strcasecmp(r->headers[i].key, "Content-Type") == 0) has_ct = 1;
        if (strcasecmp(r->headers[i].key, "Content-Length") == 0) has_cl = 1;
        used += safe_snprintf(head + used, sizeof(head) - (size_t)used,
                              "%s: %s\r\n", r->headers[i].key, r->headers[i].val);
    }
    if (!has_ct) used += safe_snprintf(head + used, sizeof(head) - (size_t)used,
                                       "Content-Type: text/plain; charset=utf-8\r\n");
    if (!has_cl) used += safe_snprintf(head + used, sizeof(head) - (size_t)used,
                                       "Content-Length: %zu\r\n", r->body_len);
    used += safe_snprintf(head + used, sizeof(head) - (size_t)used,
                          "Connection: close\r\n\r\n");
    if (used <= 0 || (size_t)used >= sizeof(head)) return -1;

    size_t sent = 0;
    while (sent < (size_t)used) {
        ssize_t w = send(c->fd, head + sent, (size_t)used - sent, MSG_NOSIGNAL);
        if (w <= 0) return -1;
        sent += (size_t)w;
    }
    sent = 0;
    while (sent < r->body_len) {
        ssize_t w = send(c->fd, r->body + sent, r->body_len - sent, MSG_NOSIGNAL);
        if (w <= 0) return -1;
        sent += (size_t)w;
    }
    return 0;
}

/* ============================================================
 * 排队项（状态码 + 响应体）
 * ============================================================ */
typedef struct {
    MHD_Response *resp;
    unsigned int  status;
} mhd_queued_t;

/* 发送队列中的全部响应 */
static int mhd_send_all(MHD_Connection *c) {
    int sent_any = 0;
    for (int i = 0; i < c->queue_count; i++) {
        mhd_queued_t *q = (mhd_queued_t *)c->queue[i];
        if (!q) continue;
        response_send(c, q->resp, q->status);
        sent_any = 1;
    }
    return sent_any;
}

/* ============================================================
 * 连接处理
 * ============================================================ */
static void conn_free(MHD_Connection *c) {
    if (!c) return;
    if (c->fd >= 0) close(c->fd);
    free(c->method); free(c->url_path); free(c->url_full); free(c->version);
    kv_free(c->headers, c->header_count);
    kv_free(c->args, c->arg_count);
    for (int i = 0; i < c->queue_count; i++) {
        mhd_queued_t *q = (mhd_queued_t *)c->queue[i];
        if (q) { MHD_destroy_response(q->resp); free(q); }
    }
    free(c->queue);
    free(c);
}

/* ============================================================
 * 启动 / 停止
 * ============================================================ */
typedef struct {
    MHD_Daemon *daemon;
    MHD_Connection *conn;
} mhd_job_t;

static void *mhd_conn_job2(void *arg);

/* ============================================================
 * accept 循环
 * ============================================================ */
static void *mhd_accept_loop(void *arg) {
    MHD_Daemon *d = (MHD_Daemon *)arg;
    LOG_INFO_T("HttpServer", "Accept", "Start", "内置 HTTP 服务器监听 :%u", d->port);

    while (d->running) {
        struct sockaddr_storage ss;
        socklen_t sl = sizeof(ss);
        int fd = accept(d->listen_fd, (struct sockaddr *)&ss, &sl);
        if (fd < 0) {
            if (errno == EINTR) continue;
            if (!d->running) break;
            usleep(50 * 1000);
            continue;
        }
        int one = 1;
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
        struct timeval tv;
        tv.tv_sec = MHD_CONN_TIMEOUT; tv.tv_usec = 0;
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

        MHD_Connection *c = calloc(1, sizeof(*c));
        if (!c) { close(fd); continue; }
        c->fd = fd;
        memcpy(&c->addr, &ss, sizeof(ss));
        c->addr_len = sl;

        mhd_job_t *job = calloc(1, sizeof(*job));
        if (!job) { conn_free(c); continue; }
        job->daemon = d;
        job->conn = c;

        pthread_t th;
        if (pthread_create(&th, NULL, mhd_conn_job2, job) != 0) {
            conn_free(c);
            free(job);
            continue;
        }
        pthread_detach(th);
    }
    LOG_INFO_T("HttpServer", "Accept", "Stop", "accept 循环退出");
    return NULL;
}

/* ============================================================
 * 真正的连接处理（含状态码记录）
 * ============================================================ */
static void *mhd_conn_job2(void *arg) {
    mhd_job_t *job = (mhd_job_t *)arg;
    MHD_Daemon *d = job->daemon;
    MHD_Connection *c = job->conn;
    free(job);

    char *head = malloc(MHD_REQ_HEAD_MAX);
    if (!head) { conn_free(c); return NULL; }

    ssize_t hl = read_request_head(c->fd, head, MHD_REQ_HEAD_MAX);
    if (hl <= 0) { free(head); conn_free(c); return NULL; }

    if (parse_request(c, head) != 0) {
        static const char err[] = "HTTP/1.1 400 Bad Request\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
        send(c->fd, err, sizeof(err) - 1, MSG_NOSIGNAL);
        free(head); conn_free(c); return NULL;
    }

    /* ---- body ---- */
    char *body = NULL;
    size_t body_len = 0;
    {
        char *hdr_end = strstr(head, "\r\n\r\n");
        size_t hdr_off = hdr_end ? (size_t)(hdr_end + 4 - head) : 0;
        size_t got = (hl > (ssize_t)hdr_off) ? ((size_t)hl - hdr_off) : 0;

        const char *cl = kv_get(c->headers, c->header_count, "Content-Length");
        long want = cl ? strtol(cl, NULL, 10) : 0;
        if (want < 0) want = 0;
        if ((unsigned long)want > MHD_BODY_MAX) want = (long)MHD_BODY_MAX;

        body_len = (size_t)want;
        if (body_len > 0) {
            body = malloc(body_len + 1);
            if (!body) { free(head); conn_free(c); return NULL; }
            size_t have = got < body_len ? got : body_len;
            if (have) memcpy(body, head + hdr_off, have);
            size_t rd = have;
            while (rd < body_len) {
                ssize_t r = recv(c->fd, body + rd, body_len - rd, 0);
                if (r <= 0) break;
                rd += (size_t)r;
            }
            body_len = rd;
            body[body_len] = '\0';
        }
    }
    free(head);

    /* ---- 三段式回调 ---- */
    void *con_cls = NULL;
    size_t ul = body_len;
    MHD_Result rc = d->rcb(d->rcb_cls, c, c->url_path, c->method, c->version,
                           NULL, &ul, &con_cls);
    if (rc == MHD_YES && con_cls && body_len > 0) {
        ul = body_len;
        rc = d->rcb(d->rcb_cls, c, c->url_path, c->method, c->version,
                    body, &ul, &con_cls);
    }
    if (rc == MHD_YES && con_cls) {
        ul = 0;
        rc = d->rcb(d->rcb_cls, c, c->url_path, c->method, c->version,
                    NULL, &ul, &con_cls);
    }
    if (body) free(body);

    /* ---- 发送 ---- */
    if (!mhd_send_all(c)) {
        /* 回调未产生响应 → 500（防御：避免连接悬挂） */
        static const char err[] =
            "HTTP/1.1 500 Internal Server Error\r\n"
            "Content-Type: application/json\r\n"
            "Content-Length: 34\r\nConnection: close\r\n\r\n"
            "{\"status\":\"error\",\"msg\":\"no response\"}";
        send(c->fd, err, sizeof(err) - 1, MSG_NOSIGNAL);
        LOG_WARN_T("HttpServer", "Job", "NoResponse", "回调未产生响应 url=%s", c->url_path);
    }

    conn_free(c);
    return NULL;
}

/* ============================================================
 * 公开 API
 * ============================================================ */
MHD_Daemon *MHD_start_daemon(unsigned int flags, unsigned short port,
                             MHD_AccessHandlerCallback apc, void *apc_cls,
                             MHD_RequestCallback rcb, void *rcb_cls, ...) {
    (void)flags; (void)apc; (void)apc_cls;

    if (!rcb) {
        LOG_ERROR_T("HttpServer", "Start", "NoCallback", "request callback is NULL");
        return NULL;
    }

    MHD_Daemon *d = calloc(1, sizeof(*d));
    if (!d) return NULL;
    d->port = port;
    d->rcb = rcb;
    d->rcb_cls = rcb_cls;
    d->running = 1;

    d->listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (d->listen_fd < 0) {
        LOG_ERROR_T("HttpServer", "Start", "SocketFail", "%s", strerror(errno));
        free(d);
        return NULL;
    }
    int one = 1;
    setsockopt(d->listen_fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

    struct sockaddr_in sa;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_addr.s_addr = htonl(INADDR_ANY);
    sa.sin_port = htons(port);

    if (bind(d->listen_fd, (struct sockaddr *)&sa, sizeof(sa)) != 0) {
        LOG_ERROR_T("HttpServer", "Start", "BindFail", "port %u: %s", port, strerror(errno));
        close(d->listen_fd);
        free(d);
        return NULL;
    }
    if (listen(d->listen_fd, MHD_LISTEN_BACKLOG) != 0) {
        LOG_ERROR_T("HttpServer", "Start", "ListenFail", "%s", strerror(errno));
        close(d->listen_fd);
        free(d);
        return NULL;
    }

    if (pthread_create(&d->accept_thread, NULL, mhd_accept_loop, d) != 0) {
        LOG_ERROR_T("HttpServer", "Start", "ThreadFail", "%s", strerror(errno));
        close(d->listen_fd);
        free(d);
        return NULL;
    }

    LOG_INFO_T("HttpServer", "Start", "OK", "listening on port %u (内置实现，无 libmicrohttpd)", port);
    return d;
}

void MHD_stop_daemon(MHD_Daemon *d) {
    if (!d) return;
    d->running = 0;
    if (d->listen_fd >= 0) {
        shutdown(d->listen_fd, SHUT_RDWR);
        close(d->listen_fd);
        d->listen_fd = -1;
    }
    /* 唤醒阻塞的 accept */
    pthread_join(d->accept_thread, NULL);
    free(d);
    LOG_INFO_T("HttpServer", "Stop", "OK", "内置 HTTP 服务器已停止");
}

const char *MHD_lookup_connection_value(MHD_Connection *c,
                                        enum MHD_ValueKind kind,
                                        const char *key) {
    if (!c || !key) return NULL;
    if (kind == MHD_GET_ARGUMENT_KIND) return kv_get(c->args, c->arg_count, key);
    if (kind == MHD_HEADER_KIND)       return kv_get(c->headers, c->header_count, key);
    if (kind == MHD_COOKIE_KIND)       return kv_get(c->headers, c->header_count, "Cookie");
    return NULL;
}

const union MHD_ConnectionInfo *MHD_get_connection_info(MHD_Connection *c,
                                                        enum MHD_ConnectionInfoType t) {
    static __thread union MHD_ConnectionInfo info;
    if (!c) return NULL;
    if (t == MHD_CONNECTION_INFO_CLIENT_ADDRESS) {
        info.client_addr = (const struct sockaddr *)&c->addr;
        return &info;
    }
    info.socket_context = NULL;
    return &info;
}

/* ---- 响应 ---- */
MHD_Response *MHD_create_response_from_buffer(size_t size, void *buffer,
                                              enum MHD_ResponseMemoryMode mode) {
    MHD_Response *r = calloc(1, sizeof(*r));
    if (!r) return NULL;
    if (mode == MHD_RESPMEM_PERSISTENT) {
        r->body = (char *)buffer;
        r->must_free = 0;
    } else if (mode == MHD_RESPMEM_MUST_FREE) {
        r->body = (char *)buffer;
        r->must_free = 1;
    } else { /* MUST_COPY */
        r->body = malloc(size + 1);
        if (!r->body) { free(r); return NULL; }
        if (buffer && size) memcpy(r->body, buffer, size);
        r->body[size] = '\0';
        r->must_free = 1;
    }
    r->body_len = size;
    return r;
}

MHD_Response *MHD_create_response_from_data(size_t size, void *data,
                                            int must_free, int must_copy) {
    return MHD_create_response_from_buffer(size, data,
        must_copy ? MHD_RESPMEM_MUST_COPY :
        (must_free ? MHD_RESPMEM_MUST_FREE : MHD_RESPMEM_PERSISTENT));
}

MHD_Result MHD_add_response_header(MHD_Response *r, const char *h, const char *v) {
    if (!r || !h) return MHD_NO;
    int cap = r->header_cap;
    if (kv_add(&r->headers, &r->header_count, &cap, h, v ? v : "") != 0) return MHD_NO;
    r->header_cap = cap;
    return MHD_YES;
}

MHD_Result MHD_add_response_footer(MHD_Response *r, const char *f, const char *v) {
    return MHD_add_response_header(r, f, v);
}

MHD_Result MHD_queue_response(MHD_Connection *c, unsigned int status, MHD_Response *r) {
    if (!c || !r) return MHD_NO;
    if (c->queue_count >= c->queue_cap) {
        int ncap = c->queue_cap ? c->queue_cap * 2 : 2;
        mhd_queued_t **nq = realloc(c->queue, (size_t)ncap * sizeof(void *));
        if (!nq) return MHD_NO;
        c->queue = (MHD_Response **)nq;
        c->queue_cap = ncap;
    }
    mhd_queued_t *q = calloc(1, sizeof(*q));
    if (!q) return MHD_NO;
    q->resp = r;
    q->status = status;
    c->queue[c->queue_count++] = (MHD_Response *)q;
    return MHD_YES;
}

void MHD_destroy_response(MHD_Response *r) {
    if (!r) return;
    if (r->must_free) free(r->body);
    kv_free(r->headers, r->header_count);
    free(r);
}
