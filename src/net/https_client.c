/**
 * @file    src/net/https_client.c
 * @brief   轻量 HTTPS 客户端实现（libssl——生命线数据源专用）
 * @version LN-0.6.2
 *
 * 实现要点：
 *   · TLS_client_method（OpenSSL 1.1+/3.x 通用）；SNI 设置
 *   · HTTP/1.1 + Connection: close + Accept-Encoding: identity（免 gzip）
 *   · 读循环动态增长（realloc）；chunked 解码；Content-Length 读取
 *   · 全路径错误检查（防弹/防御范式）
 */

#include "https_client.h"
#include "egress.h"   /* 【0.7.0 S2】出口白名单 */
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
#include <sys/socket.h>
#include <sys/types.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>

#include <openssl/ssl.h>
#include <openssl/err.h>

/* 简易大小写不敏感 strstr（避免依赖 _GNU_SOURCE） */
static char *strcasestr_dummy(const char *hay, const char *needle) {
    if (!hay || !needle) return NULL;
    size_t nl = strlen(needle);
    if (!nl) return (char *)hay;
    for (const char *p = hay; *p; p++) {
        if (strncasecmp(p, needle, nl) == 0) return (char *)p;
    }
    return NULL;
}

/* ============================================================
 * URL 解析（https://host[:port]/path）
 * ============================================================ */
typedef struct {
    int  is_https;
    char host[256];
    int  port;
    char path[2048];   /* 含 query */
} hs_url_t;

static int hs_parse_url(const char *url, hs_url_t *out) {
    if (!url || !out) return -1;
    memset(out, 0, sizeof(*out));

    const char *p = strstr(url, "://");
    if (!p) return -1;
    size_t sl = (size_t)(p - url);
    if (sl >= 8) return -1;
    char scheme[8] = {0};
    memcpy(scheme, url, sl);
    out->is_https = (strcasecmp(scheme, "https") == 0);
    if (!out->is_https && strcasecmp(scheme, "http") != 0) return -1;

    const char *host_start = p + 3;
    const char *path_start = strchr(host_start, '/');
    const char *host_end = path_start ? path_start : (host_start + strlen(host_start));
    const char *colon = memchr(host_start, ':', (size_t)(host_end - host_start));

    size_t hl;
    if (colon) {
        hl = (size_t)(colon - host_start);
        int pp = atoi(colon + 1);
        out->port = pp > 0 ? pp : (out->is_https ? 443 : 80);
    } else {
        hl = (size_t)(host_end - host_start);
        out->port = out->is_https ? 443 : 80;
    }
    if (hl == 0 || hl >= sizeof(out->host)) return -1;
    memcpy(out->host, host_start, hl);
    out->host[hl] = '\0';

    if (path_start) {
        safe_strncpy(out->path, path_start, sizeof(out->path));
    } else {
        safe_strncpy(out->path, "/", sizeof(out->path));
    }
    return 0;
}

/* ============================================================
 * TCP 连接（带超时——非阻塞 connect + select）
 * ============================================================ */
static int hs_tcp_connect(const char *host, int port, int timeout_s) {
    struct addrinfo hints, *res = NULL, *rp = NULL;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    char portstr[16];
    safe_snprintf(portstr, sizeof(portstr), "%d", port);

    if (getaddrinfo(host, portstr, &hints, &res) != 0 || !res) {
        LOG_WARN_T("Https", "Connect", "DNSFail", "getaddrinfo('%s') failed", host);
        return -1;
    }

    int fd = -1;
    for (rp = res; rp; rp = rp->ai_next) {
        fd = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
        if (fd < 0) continue;

        /* 非阻塞 connect 带超时 */
        int flags = fcntl(fd, F_GETFL, 0);
        fcntl(fd, F_SETFL, flags | O_NONBLOCK);
        int rc = connect(fd, rp->ai_addr, rp->ai_addrlen);
        if (rc == 0) {
            fcntl(fd, F_SETFL, flags);
            break;
        }
        if (errno == EINPROGRESS) {
            fd_set wfds;
            FD_ZERO(&wfds);
            FD_SET(fd, &wfds);
            struct timeval tv;
            tv.tv_sec = timeout_s > 0 ? timeout_s : 10;
            tv.tv_usec = 0;
            rc = select(fd + 1, NULL, &wfds, NULL, &tv);
            if (rc > 0) {
                int soerr = 0;
                socklen_t sl2 = sizeof(soerr);
                getsockopt(fd, SOL_SOCKET, SO_ERROR, &soerr, &sl2);
                if (soerr == 0) {
                    fcntl(fd, F_SETFL, flags);
                    break;
                }
            }
        }
        close(fd);
        fd = -1;
    }
    freeaddrinfo(res);
    if (fd < 0) {
        LOG_WARN_T("Https", "Connect", "TCPFail", "connect %s:%d failed", host, port);
        return -1;
    }

    /* 读写超时（SSL_read 底层 socket） */
    struct timeval tv;
    tv.tv_sec = timeout_s > 0 ? timeout_s : 10;
    tv.tv_usec = 0;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    return fd;
}

/* ============================================================
 * 缓冲区增长
 * ============================================================ */
typedef struct {
    char  *buf;
    size_t len;
    size_t cap;
    size_t max;
} hs_buf_t;

static int hs_buf_append(hs_buf_t *b, const char *data, size_t n) {
    if (b->len + n + 1 > b->max) {
        n = (b->len + 1 < b->max) ? (b->max - b->len - 1) : 0;
    }
    if (n == 0) return 0;   /* 到上限——静默截断（防爆内存） */
    if (b->len + n + 1 > b->cap) {
        size_t ncap = b->cap ? b->cap : 8192;
        while (ncap < b->len + n + 1) ncap *= 2;
        if (ncap > b->max) ncap = b->max;
        char *nb = realloc(b->buf, ncap);
        if (!nb) return -1;
        b->buf = nb;
        b->cap = ncap;
    }
    memcpy(b->buf + b->len, data, n);
    b->len += n;
    b->buf[b->len] = '\0';
    return 1;
}

/* ============================================================
 * chunked 解码（就地——返回解码后长度）
 * ============================================================ */
static size_t hs_dechunk(char *body, size_t len) {
    size_t in = 0, out = 0;
    while (in < len) {
        /* 解析十六进制长度 */
        size_t sz = 0;
        int digits = 0;
        while (in < len && isxdigit((unsigned char)body[in])) {
            char c = body[in];
            sz = sz * 16 + (size_t)(isdigit((unsigned char)c) ? c - '0'
                                                              : (tolower((unsigned char)c) - 'a' + 10));
            in++; digits++;
            if (digits > 8) return out;
        }
        /* 跳过分号扩展 + CRLF */
        while (in < len && body[in] != '\n') in++;
        if (in < len) in++;
        if (sz == 0) break;   /* 终止块 */
        if (in + sz > len) break;
        memmove(body + out, body + in, sz);
        out += sz;
        in += sz;
        if (in + 1 < len && body[in] == '\r') in++;
        if (in < len && body[in] == '\n') in++;
    }
    if (out < len) body[out] = '\0';
    return out;
}

/* ============================================================
 * 核心：HTTPS 请求
 * ============================================================ */
static char *hs_request(const char *method, const char *url,
                        const char *body, const char *content_type,
                        size_t max_sz, int timeout_s, int verify, int *http_code) {
    if (http_code) *http_code = 0;
    if (!url) return NULL;
    if (max_sz == 0) max_sz = 1024 * 1024;
    if (timeout_s <= 0) timeout_s = 12;

    /* 【0.7.0 S2】出口白名单检查（生命线数据源均在内置清单） */
    if (egress_check_url(url) != 0) {
        LOG_WARN_T("Https", "Request", "EgressBlocked", "blocked by egress allowlist: %s", url);
        return NULL;
    }

    hs_url_t u;
    if (hs_parse_url(url, &u) != 0) {
        LOG_WARN_T("Https", "Request", "BadURL", "cannot parse '%s'", url);
        return NULL;
    }

    int fd = hs_tcp_connect(u.host, u.port, timeout_s);
    if (fd < 0) return NULL;

    SSL_CTX *ctx = NULL;
    SSL *ssl = NULL;
    char *result = NULL;

    if (u.is_https) {
        /* OpenSSL 1.1+ 自动初始化 */
        ctx = SSL_CTX_new(TLS_client_method());
        if (!ctx) {
            close(fd);
            return NULL;
        }
        SSL_CTX_set_min_proto_version(ctx, TLS1_2_VERSION);
        SSL_CTX_set_mode(ctx, SSL_MODE_AUTO_RETRY);
        if (verify) {
            SSL_CTX_set_default_verify_paths(ctx);
            SSL_CTX_set_verify(ctx, SSL_VERIFY_PEER, NULL);
        } else {
            SSL_CTX_set_verify(ctx, SSL_VERIFY_NONE, NULL);
        }
        ssl = SSL_new(ctx);
        if (!ssl) {
            SSL_CTX_free(ctx);
            close(fd);
            return NULL;
        }
        SSL_set_fd(ssl, fd);
        SSL_set_tlsext_host_name(ssl, u.host);   /* SNI */
        if (verify) {
            SSL_set1_host(ssl, u.host);
        }
        if (SSL_connect(ssl) != 1) {
            unsigned long e = ERR_get_error();
            char ebuf[256] = {0};
            if (e) ERR_error_string_n(e, ebuf, sizeof(ebuf));
            LOG_WARN_T("Https", "TLS", "HandshakeFail", "%s:%d (%s)", u.host, u.port,
                       ebuf[0] ? ebuf : "unknown");
            SSL_free(ssl);
            SSL_CTX_free(ctx);
            close(fd);
            return NULL;
        }
        if (verify) {
            long vr = SSL_get_verify_result(ssl);
            if (vr != X509_V_OK) {
                LOG_WARN_T("Https", "TLS", "CertInvalid", "%s cert verify: %ld", u.host, vr);
                SSL_free(ssl);
                SSL_CTX_free(ctx);
                close(fd);
                return NULL;
            }
        }
    }

    /* ---- 构造请求 ---- */
    char header[4096];
    size_t blen = body ? strlen(body) : 0;
    int hlen;
    if (body) {
        hlen = safe_snprintf(header, sizeof(header),
            "%s %s HTTP/1.1\r\n"
            "Host: %s\r\n"
            "User-Agent: LINGOS/0.6.2\r\n"
            "Accept: application/json, text/plain, */*\r\n"
            "Accept-Encoding: identity\r\n"
            "Content-Type: %s\r\n"
            "Content-Length: %zu\r\n"
            "Connection: close\r\n"
            "\r\n",
            method, u.path, u.host, content_type ? content_type : "application/json", blen);
    } else {
        hlen = safe_snprintf(header, sizeof(header),
            "%s %s HTTP/1.1\r\n"
            "Host: %s\r\n"
            "User-Agent: LINGOS/0.6.2\r\n"
            "Accept: application/json, text/plain, */*\r\n"
            "Accept-Encoding: identity\r\n"
            "Connection: close\r\n"
            "\r\n",
            method, u.path, u.host);
    }
    if (hlen <= 0 || (size_t)hlen >= sizeof(header)) goto cleanup;

    /* ---- 发送 ---- */
    {
        const char *out = header;
        size_t want = (size_t)hlen, sent = 0;
        /* 先发头 */
        while (sent < want) {
            ssize_t w = u.is_https ? SSL_write(ssl, out + sent, (int)(want - sent))
                                   : write(fd, out + sent, want - sent);
            if (w <= 0) goto cleanup;
            sent += (size_t)w;
        }
        /* 再发体 */
        if (body && blen) {
            size_t bs = 0;
            while (bs < blen) {
                ssize_t w = u.is_https ? SSL_write(ssl, body + bs, (int)(blen - bs))
                                       : write(fd, body + bs, blen - bs);
                if (w <= 0) goto cleanup;
                bs += (size_t)w;
            }
        }
    }

    /* ---- 接收 ---- */
    {
        hs_buf_t b = {0};
        b.max = max_sz + 64;   /* 头 + 体 */
        char tmp[16384];
        for (;;) {
            ssize_t r;
            if (u.is_https) {
                r = SSL_read(ssl, tmp, sizeof(tmp));
                if (r <= 0) {
                    int se = SSL_get_error(ssl, (int)r);
                    if (se == SSL_ERROR_WANT_READ || se == SSL_ERROR_WANT_WRITE) continue;
                    break;   /* ZERO_RETURN / 错误 → 结束 */
                }
            } else {
                r = read(fd, tmp, sizeof(tmp));
                if (r <= 0) {
                    if (r < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) break;
                    break;
                }
            }
            if (hs_buf_append(&b, tmp, (size_t)r) <= 0) break;
        }

        if (!b.buf) goto cleanup;

        /* ---- 解析HTTP响应 ---- */
        char *hdr_end = strstr(b.buf, "\r\n\r\n");
        if (!hdr_end) goto cleanup;

        int code = 0;
        if (sscanf(b.buf, "HTTP/%*s %d", &code) != 1) goto cleanup;
        if (http_code) *http_code = code;

        char *bodyp = hdr_end + 4;
        size_t bodylen = b.len - (size_t)(bodyp - b.buf);

        /* chunked? */
        int chunked = 0;
        {
            /* 只在响应头区域找 Transfer-Encoding（避免 body 中误匹配） */
            size_t hdr_len = (size_t)(hdr_end - b.buf);
            char save = b.buf[hdr_len];
            b.buf[hdr_len] = '\0';
            chunked = (strcasestr_dummy(b.buf, "Transfer-Encoding: chunked") != NULL);
            b.buf[hdr_len] = save;
        }
        if (chunked) {
            bodylen = hs_dechunk(bodyp, bodylen);
        } else {
            /* Content-Length 截断（有明确长度用明确长度） */
            char *cl = NULL;
            {
                size_t hdr_len = (size_t)(hdr_end - b.buf);
                char save = b.buf[hdr_len];
                b.buf[hdr_len] = '\0';
                cl = strcasestr_dummy(b.buf, "Content-Length:");
                if (cl) {
                    char *val = cl + strlen("Content-Length:");
                    while (*val == ' ') val++;
                    long clv = atol(val);
                    if (clv >= 0 && (size_t)clv <= bodylen) bodylen = (size_t)clv;
                }
                b.buf[hdr_len] = save;
            }
        }

        result = malloc(bodylen + 1);
        if (result) {
            memcpy(result, bodyp, bodylen);
            result[bodylen] = '\0';
        }
        free(b.buf);
    }

cleanup:
    if (u.is_https) {
        if (ssl) { SSL_shutdown(ssl); SSL_free(ssl); }
        if (ctx) SSL_CTX_free(ctx);
    }
    close(fd);
    return result;
}

char *https_get_alloc(const char *url, size_t max_sz, int timeout_s,
                      int verify, int *http_code) {
    return hs_request("GET", url, NULL, NULL, max_sz, timeout_s, verify, http_code);
}

char *https_post_json_alloc(const char *url, const char *json, size_t max_sz,
                            int timeout_s, int verify, int *http_code) {
    return hs_request("POST", url, json, "application/json",
                      max_sz, timeout_s, verify, http_code);
}
