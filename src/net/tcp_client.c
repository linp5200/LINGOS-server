/**
 * @file    tcp_client.c
 * @brief   TCP 客户端（用于连接 Ollama/DeepSeek 代理）
 * @version LN-B-5.0.0.0
 * @changes 安全字符串替换；双文支持
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <errno.h>
#include <sys/time.h>
#include "tcp_client.h"
#include "../common/safe_string.h"
#include "../common/lang.h"
#include "../lib/log_extra.h"

#define TCP_HTTP_BUF_SIZE   (4 * 1024 * 1024)
#define TCP_RECV_BUF_SIZE   (256 * 1024)

void tcp_client_init(void) {
    LOG_INFO_T("TCPClient", "Init", "Start", "TCP client ready (idle)");
}

int tcp_send_recv(const char *host, uint16_t port,
                  const char *body, char *resp, uint32_t resp_len,
                  int timeout_ms) {
    LOG_INFO_T("TCPClient", "SendRecv", "Enter", "host=%s port=%d timeout=%dms",
               host, port, timeout_ms);

    if (!host || !body || !resp || resp_len == 0) {
        LOG_ERROR_T("TCPClient", "SendRecv", "Error", tr("Invalid arguments", "无效参数"));
        return -1;
    }

    /* DNS 解析 */
    struct addrinfo hints, *result, *rp;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;

    char port_str[8];
    safe_snprintf(port_str, sizeof(port_str), "%d", port);

    int s = getaddrinfo(host, port_str, &hints, &result);
    if (s != 0) {
        LOG_ERROR_T("TCPClient", "DNS", "Fail", "getaddrinfo %s: %s", host, gai_strerror(s));
        return -1;
    }

    int fd = -1;
    for (rp = result; rp != NULL; rp = rp->ai_next) {
        fd = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
        if (fd < 0) continue;

        if (timeout_ms > 0) {
            struct timeval tv;
            tv.tv_sec = timeout_ms / 1000;
            tv.tv_usec = (timeout_ms % 1000) * 1000;
            setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
            setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
        }

        if (connect(fd, rp->ai_addr, rp->ai_addrlen) == 0) {
            break;
        }
        close(fd);
        fd = -1;
    }
    freeaddrinfo(result);

    if (fd < 0) {
        LOG_ERROR_T("TCPClient", "Connect", "Error", "connect to %s:%d failed: %s", host, port, strerror(errno));
        return -1;
    }

    LOG_INFO_T("TCPClient", "Connect", "OK", "Connected to %s:%d", host, port);

    static char http_request[TCP_HTTP_BUF_SIZE];
    size_t body_len = strlen(body);
    int req_len = safe_snprintf(http_request, sizeof(http_request),
        "POST /generate HTTP/1.0\r\n"
        "Host: %s:%d\r\n"
        "Content-Type: application/json\r\n"
        "Content-Length: %zu\r\n"
        "\r\n"
        "%s",
        host, port, body_len, body);

    if (req_len < 0 || (size_t)req_len >= sizeof(http_request)) {
        LOG_ERROR_T("TCPClient", "Send", "Error", "HTTP request too long (%d bytes), max %zu", req_len, sizeof(http_request));
        close(fd);
        return -1;
    }

    LOG_INFO_T("TCPClient", "Send", "Prepare", "body_len=%zu, total_request_len=%d", body_len, req_len);

    size_t total_sent = 0;
    while (total_sent < (size_t)req_len) {
        ssize_t sent_now = send(fd, http_request + total_sent, (size_t)req_len - total_sent, 0);
        if (sent_now < 0) {
            LOG_ERROR_T("TCPClient", "Send", "Error", "send failed: %s", strerror(errno));
            close(fd);
            return -1;
        }
        total_sent += sent_now;
    }

    LOG_INFO_T("TCPClient", "Send", "Done", "Sent %zu bytes", total_sent);

    uint32_t total_recv = 0;
    while (total_recv < resp_len - 1) {
        ssize_t n;
        do {
            n = recv(fd, resp + total_recv, resp_len - total_recv - 1, 0);
        } while (n < 0 && errno == EINTR);

        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                LOG_WARN_T("TCPClient", "Recv", "Timeout", "Timeout after %dms", timeout_ms);
                close(fd);
                return -2;
            }
            LOG_ERROR_T("TCPClient", "Recv", "Error", "recv error: %s", strerror(errno));
            close(fd);
            return -1;
        }
        if (n == 0) {
            LOG_INFO_T("TCPClient", "Recv", "Closed", tr("Server closed connection", "服务器关闭连接"));
            break;
        }
        total_recv += n;
    }
    resp[total_recv] = '\0';
    LOG_INFO_T("TCPClient", "Recv", "Done", "Total %u bytes received", total_recv);
    close(fd);
    return 0;
}