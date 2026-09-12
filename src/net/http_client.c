/**
 * @file    src/net/http_client.c
 * @brief   轻量 HTTP 客户端实现（纯 socket + dlopen curl）
 * @version LN-0.5.0
 *
 * 设计：
 *   ① 纯 socket 分支 —— 零外部依赖，覆盖「内网上报」场景（本系统 90% 需求）
 *   ② dlopen 分支   —— libcurl 变为**可选运行时依赖**（需要 https/断点续传时）
 *
 * 为什么不用 `-lcurl`：
 *   Ubuntu 22.04 的 libcurl 传递依赖 7 个版本敏感 soname
 *   （libldap-2.5 / liblber-2.5 / libavcodec.58 / libavformat.58 /
 *     libswscale.5 / libavutil.56 / libunistring.2），
 *   在 25.10 上全部不存在 → 二进制无法启动。
 *   本模块让这些依赖**彻底消失**（内网用 socket，下载用 dlopen）。
 */

#include "http_client.h"
#include "../lib/log_extra.h"
#include "../common/safe_string.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <errno.h>
#include <ctype.h>
#include <fcntl.h>
#include <time.h>
#include <dlfcn.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>

/* 不区分大小写的子串查找（避免依赖 _GNU_SOURCE 的 strcasestr） */
static char *find_ci(const char *hay, const char *needle) {
    if (!hay || !needle) return NULL;
    size_t nl = strlen(needle);
    if (nl == 0) return (char *)hay;
    for (const char *p = hay; *p; p++) {
        if (strncasecmp(p, needle, nl) == 0) return (char *)p;
    }
    return NULL;
}

/* ============================================================
 * URL 解析
 * ============================================================ */
typedef struct {
    char scheme[16];
    char host[256];
    int  port;
    char path[2048];   /* 含 query */
    int  is_https;
} url_parts_t;

static int parse_url(const char *url, url_parts_t *out) {
    if (!url || !out) return -1;
    memset(out, 0, sizeof(*out));

    const char *p = strstr(url, "://");
    if (!p) return -1;
    size_t sl = (size_t)(p - url);
    if (sl >= sizeof(out->scheme)) sl = sizeof(out->scheme) - 1;
    memcpy(out->scheme, url, sl);
    out->scheme[sl] = '\0';
    for (size_t i = 0; i < sl; i++) out->scheme[i] = (char)tolower((unsigned char)out->scheme[i]);

    out->is_https = (strcmp(out->scheme, "https") == 0);
    if (strcmp(out->scheme, "http") != 0 && !out->is_https) {
        LOG_WARN_T("HttpClient", "ParseUrl", "BadScheme", "%s", out->scheme);
        return -1;
    }

    const char *hp = p + 3;
    const char *slash = strchr(hp, '/');
    size_t hl = slash ? (size_t)(slash - hp) : strlen(hp);
    if (hl >= sizeof(out->host)) hl = sizeof(out->host) - 1;

    /* 分离 host:port */
    char hostport[300];
    if (hl >= sizeof(hostport)) hl = sizeof(hostport) - 1;
    memcpy(hostport, hp, hl);
    hostport[hl] = '\0';

    char *colon = strrchr(hostport, ':');
    if (colon && strchr(hostport, '[') == NULL) {
        *colon = '\0';
        out->port = atoi(colon + 1);
    }
    safe_strncpy(out->host, hostport, sizeof(out->host));
    if (out->port <= 0) out->port = out->is_https ? 443 : 80;

    if (slash) {
        safe_strncpy(out->path, slash, sizeof(out->path));
    } else {
        safe_strncpy(out->path, "/", sizeof(out->path));
    }
    return 0;
}

/* ============================================================
 * 一、纯 socket HTTP
 * ============================================================ */

/* 连接（含 DNS + 超时） */
static int tcp_connect(const char *host, int port, int timeout_s) {
    char portstr[16];
    safe_snprintf(portstr, sizeof(portstr), "%d", port);

    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    int gai = getaddrinfo(host, portstr, &hints, &res);
    if (gai != 0) {
        LOG_WARN_T("HttpClient", "Connect", "DnsFail", "%s: %s", host, gai_strerror(gai));
        return -1;
    }

    int fd = -1;
    for (struct addrinfo *ai = res; ai; ai = ai->ai_next) {
        fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (fd < 0) continue;

        struct timeval tv;
        tv.tv_sec = timeout_s > 0 ? timeout_s : HTTP_DEFAULT_TIMEOUT;
        tv.tv_usec = 0;
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

        if (connect(fd, ai->ai_addr, ai->ai_addrlen) == 0) {
            int one = 1;
            setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
            break;
        }
        close(fd);
        fd = -1;
    }
    freeaddrinfo(res);
    if (fd < 0)
        LOG_WARN_T("HttpClient", "Connect", "Fail", "%s:%d", host, port);
    return fd;
}

/* 读完整响应（分离头与体） */
static int read_http_response(int fd, char *resp, size_t resp_sz, int *out_status) {
    char buf[8192];
    size_t total = 0;
    char *all = malloc(1024 * 1024);   /* 上限 1MB */
    if (!all) return -1;

    for (;;) {
        ssize_t n = recv(fd, buf, sizeof(buf) - 1, 0);
        if (n > 0) {
            if (total + (size_t)n >= 1024 * 1024) break;
            memcpy(all + total, buf, (size_t)n);
            total += (size_t)n;
            all[total] = '\0';
        } else {
            break;   /* 0=对端关闭；<0=超时/错误 */
        }
        /* Content-Length 满足即结束 */
        char *cl = find_ci(all, "content-length:");
        if (cl) {
            long want = strtol(cl + 15, NULL, 10);
            char *hdrend = strstr(all, "\r\n\r\n");
            if (hdrend && want >= 0 && (long)(total - (size_t)(hdrend + 4 - all)) >= want) break;
        }
        /* 无 Content-Length：Connection: close 场景，继续读到关闭 */
    }

    /* 解析状态行 */
    int status = 0;
    if (total > 5 && strncmp(all, "HTTP/", 5) == 0) {
        char *sp = strchr(all, ' ');
        if (sp) status = atoi(sp + 1);
    }
    if (out_status) *out_status = status;

    /* 取 body */
    char *body = strstr(all, "\r\n\r\n");
    if (body) {
        body += 4;
        size_t blen = total - (size_t)(body - all);
        if (resp && resp_sz) {
            size_t cp = blen < resp_sz - 1 ? blen : resp_sz - 1;
            memcpy(resp, body, cp);
            resp[cp] = '\0';
        }
    } else if (resp && resp_sz) {
        resp[0] = '\0';
    }
    free(all);
    return 0;
}

int http_request(const char *method, const char *url,
                 const char *body, const char *content_type,
                 char *resp, size_t resp_sz, int timeout_s) {
    if (!method || !url) return -1;
    if (resp && resp_sz) resp[0] = '\0';

    url_parts_t u;
    if (parse_url(url, &u) != 0) return -1;

    /* https → 交给 dlopen curl */
    if (u.is_https) {
        if (http_curl_available() == 0) {
            LOG_WARN_T("HttpClient", "Request", "NoTlsSupport",
                       "https 需要 libcurl（当前不可用）: %s", url);
            return -1;
        }
        /* 简化：https 仅用于下载，此处不支持通用 https 请求 */
        LOG_WARN_T("HttpClient", "Request", "HttpsUnsupported",
                   "通用 https 请求请使用 http_download 或 Python 侧: %s", url);
        return -1;
    }

    int fd = tcp_connect(u.host, u.port, timeout_s);
    if (fd < 0) return -1;

    size_t blen = body ? strlen(body) : 0;
    const char *ct = content_type ? content_type : "application/json";

    char head[4096];
    int hn;
    if (blen > 0) {
        hn = safe_snprintf(head, sizeof(head),
            "%s %s HTTP/1.1\r\n"
            "Host: %s:%d\r\n"
            "User-Agent: LINGOS/0.5\r\n"
            "Accept: */*\r\n"
            "Content-Type: %s\r\n"
            "Content-Length: %zu\r\n"
            "Connection: close\r\n"
            "\r\n",
            method, u.path, u.host, u.port, ct, blen);
    } else {
        hn = safe_snprintf(head, sizeof(head),
            "%s %s HTTP/1.1\r\n"
            "Host: %s:%d\r\n"
            "User-Agent: LINGOS/0.5\r\n"
            "Accept: */*\r\n"
            "Connection: close\r\n"
            "\r\n",
            method, u.path, u.host, u.port);
    }
    if (hn <= 0) { close(fd); return -1; }

    /* 发送 */
    size_t sent = 0;
    while (sent < (size_t)hn) {
        ssize_t w = send(fd, head + sent, (size_t)hn - sent, 0);
        if (w <= 0) { close(fd); return -1; }
        sent += (size_t)w;
    }
    if (blen > 0) {
        sent = 0;
        while (sent < blen) {
            ssize_t w = send(fd, body + sent, blen - sent, 0);
            if (w <= 0) { close(fd); return -1; }
            sent += (size_t)w;
        }
    }

    int status = 0;
    read_http_response(fd, resp, resp_sz, &status);
    close(fd);

    if (status == 0) return -1;
    if (status >= 200 && status < 300) {
        LOG_DEBUG_T("HttpClient", "Request", "OK", "%s %s → %d", method, u.path, status);
        return 0;
    }
    LOG_WARN_T("HttpClient", "Request", "HttpErr", "%s %s → %d", method, u.path, status);
    return status;
}

int http_post_json(const char *url, const char *json,
                   char *resp, size_t resp_sz, int timeout_s) {
    return http_request("POST", url, json, "application/json",
                        resp, resp_sz, timeout_s);
}

int http_get(const char *url, char *resp, size_t resp_sz, int timeout_s) {
    return http_request("GET", url, NULL, NULL, resp, resp_sz, timeout_s);
}

/* ============================================================
 * 二、dlopen libcurl（可选依赖）
 * ============================================================ */
typedef void    CURL;
typedef int     CURLcode;
typedef int     CURLoption;

/* curl 常量（ABI 稳定，取自 curl/curl.h —— 避免编译期依赖头文件） */
#define CURLE_OK                0
#define CURLOPT_WRITEDATA       10001
#define CURLOPT_URL             10002
#define CURLOPT_NOSIGNAL        99
#define CURLOPT_TIMEOUT         13
#define CURLOPT_CONNECTTIMEOUT  78
#define CURLOPT_USERAGENT       10018
#define CURLOPT_WRITEFUNCTION   20011
#define CURLOPT_FOLLOWLOCATION  52
#define CURLOPT_SSL_VERIFYPEER  64
#define CURLOPT_SSL_VERIFYHOST  81
#define CURLOPT_FAILONERROR     45

typedef CURL*    (*fn_easy_init)(void);
typedef CURLcode (*fn_easy_setopt)(CURL*, CURLoption, ...);
typedef CURLcode (*fn_easy_perform)(CURL*);
typedef void     (*fn_easy_cleanup)(CURL*);
typedef const char* (*fn_easy_strerror)(CURLcode);

static void           *g_curl_lib   = NULL;
static int             g_curl_tried = 0;
static fn_easy_init    g_easy_init  = NULL;
static fn_easy_setopt  g_easy_setopt = NULL;
static fn_easy_perform g_easy_perform = NULL;
static fn_easy_cleanup g_easy_cleanup = NULL;

/* 尝试常见 soname（覆盖 22.04 → 25.10 的命名差异） */
static const char *CURL_SONAMES[] = {
    "libcurl.so.4",          /* 绝大多数发行版 */
    "libcurl-gnutls.so.4",
    "libcurl.so",
    "libcurl.a",
    NULL
};

static int curl_lazy_load(void) {
    if (g_curl_tried) return g_curl_lib != NULL;
    g_curl_tried = 1;

    for (int i = 0; CURL_SONAMES[i]; i++) {
        void *h = dlopen(CURL_SONAMES[i], RTLD_LAZY | RTLD_LOCAL);
        if (!h) continue;
        g_easy_init    = (fn_easy_init)dlsym(h, "curl_easy_init");
        g_easy_setopt  = (fn_easy_setopt)dlsym(h, "curl_easy_setopt");
        g_easy_perform = (fn_easy_perform)dlsym(h, "curl_easy_perform");
        g_easy_cleanup = (fn_easy_cleanup)dlsym(h, "curl_easy_cleanup");
        if (g_easy_init && g_easy_setopt && g_easy_perform && g_easy_cleanup) {
            g_curl_lib = h;
            LOG_INFO_T("HttpClient", "CurlLoad", "OK", "%s（可选依赖已就绪）", CURL_SONAMES[i]);
            return 1;
        }
        dlclose(h);
    }
    LOG_INFO_T("HttpClient", "CurlLoad", "Unavailable",
               "libcurl 不可用 —— 将使用纯 socket（仅 http://，https 需 libcurl）");
    return 0;
}

int http_curl_available(void) { return curl_lazy_load(); }

/* curl 写回调 */
typedef struct {
    FILE *fp;
    char *mem;
    size_t mem_len;
    size_t mem_cap;
    int    to_mem;
} curl_sink_t;

static size_t curl_write_cb(char *ptr, size_t size, size_t nmemb, void *userdata) {
    curl_sink_t *s = (curl_sink_t *)userdata;
    size_t n = size * nmemb;
    if (s->to_mem) {
        if (s->mem_len + n < s->mem_cap) {
            memcpy(s->mem + s->mem_len, ptr, n);
            s->mem_len += n;
            s->mem[s->mem_len] = '\0';
        }
        return n;
    }
    return fwrite(ptr, 1, n, s->fp);
}

int http_download(const char *url, const char *out_path, int timeout_s) {
    if (!url || !out_path) return -1;

    url_parts_t u;
    if (parse_url(url, &u) != 0) return -1;

    /* 有 curl → 用 curl（支持 https + 跟随重定向） */
    if (curl_lazy_load()) {
        FILE *fp = fopen(out_path, "wb");
        if (!fp) return -1;

        CURL *h = g_easy_init();
        if (!h) { fclose(fp); return -1; }

        curl_sink_t sink;
        memset(&sink, 0, sizeof(sink));
        sink.fp = fp;
        sink.to_mem = 0;

        int to = timeout_s > 0 ? timeout_s : 60;
        g_easy_setopt(h, CURLOPT_URL, url);
        g_easy_setopt(h, CURLOPT_WRITEFUNCTION, curl_write_cb);
        g_easy_setopt(h, CURLOPT_WRITEDATA, &sink);
        g_easy_setopt(h, CURLOPT_TIMEOUT, to);
        g_easy_setopt(h, CURLOPT_CONNECTTIMEOUT, 10);
        g_easy_setopt(h, CURLOPT_FOLLOWLOCATION, 1);
        g_easy_setopt(h, CURLOPT_USERAGENT, "LINGOS/0.5");
        g_easy_setopt(h, CURLOPT_NOSIGNAL, 1);
        /* 保持默认证书校验（安全）；自签场景由用户在配置中处理 */
        g_easy_setopt(h, CURLOPT_SSL_VERIFYPEER, 1);
        g_easy_setopt(h, CURLOPT_SSL_VERIFYHOST, 2);

        CURLcode rc = g_easy_perform(h);
        g_easy_cleanup(h);
        fclose(fp);

        if (rc != CURLE_OK) {
            LOG_WARN_T("HttpClient", "Download", "CurlFail", "%s rc=%d", url, rc);
            unlink(out_path);
            return -1;
        }
        LOG_INFO_T("HttpClient", "Download", "OK", "%s → %s (curl)", url, out_path);
        return 0;
    }

    /* 无 curl → 纯 socket（仅 http） */
    if (u.is_https) {
        LOG_ERROR_T("HttpClient", "Download", "HttpsNoCurl",
                    "https 下载需要 libcurl，当前不可用: %s", url);
        return -1;
    }

    LOG_WARN_T("HttpClient", "Download", "SocketFallback",
               "回退纯 socket 下载（不支持重定向/断点）: %s", url);

    int fd = tcp_connect(u.host, u.port, timeout_s > 0 ? timeout_s : 60);
    if (fd < 0) return -1;

    char req[3072];
    int rn = safe_snprintf(req, sizeof(req),
        "GET %s HTTP/1.1\r\n"
        "Host: %s:%d\r\n"
        "User-Agent: LINGOS/0.5\r\n"
        "Accept: */*\r\n"
        "Connection: close\r\n\r\n",
        u.path, u.host, u.port);
    if (rn <= 0) { close(fd); return -1; }

    size_t sent = 0;
    while (sent < (size_t)rn) {
        ssize_t w = send(fd, req + sent, (size_t)rn - sent, 0);
        if (w <= 0) { close(fd); return -1; }
        sent += (size_t)w;
    }

    /* 先读头，再流式写体 */
    FILE *fp = fopen(out_path, "wb");
    if (!fp) { close(fd); return -1; }

    char hdr[16384];
    size_t hlen = 0;
    int status = 0;
    char *body_start = NULL;
    size_t body_have = 0;

    /* 逐块读，直到找到头部结束 */
    while (hlen < sizeof(hdr) - 1) {
        ssize_t n = recv(fd, hdr + hlen, sizeof(hdr) - 1 - hlen, 0);
        if (n <= 0) break;
        hlen += (size_t)n;
        hdr[hlen] = '\0';
        char *e = strstr(hdr, "\r\n\r\n");
        if (e) { body_start = e + 4; break; }
    }

    int ok = 0;
    if (body_start) {
        char *sp = strchr(hdr, ' ');
        if (sp) status = atoi(sp + 1);
        body_have = hlen - (size_t)(body_start - hdr);
        if (status >= 200 && status < 300) {
            if (body_have) fwrite(body_start, 1, body_have, fp);
            for (;;) {
                char blk[8192];
                ssize_t n = recv(fd, blk, sizeof(blk), 0);
                if (n <= 0) break;
                fwrite(blk, 1, (size_t)n, fp);
            }
            ok = 1;
        }
    }
    fclose(fp);
    close(fd);

    if (!ok) {
        LOG_WARN_T("HttpClient", "Download", "HttpErr", "%s status=%d", url, status);
        unlink(out_path);
        return -1;
    }
    LOG_INFO_T("HttpClient", "Download", "OK", "%s → %s (socket)", url, out_path);
    return 0;
}

/* ============================================================
 * 三、诊断
 * ============================================================ */
void http_report_capabilities(void) {
    LOG_INFO_T("HttpClient", "Capabilities", "Report",
               "内置 socket HTTP: 可用 | libcurl: %s",
               curl_lazy_load() ? "可用（https/重定向）" : "不可用（仅 http://）");
}
