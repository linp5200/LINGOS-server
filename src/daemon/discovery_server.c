/**
 * @file    discovery_server.c
 * @brief   UDP 局域网发现服务 - 响应 App 的 LINGOS-DISCOVER 广播
 * @version LN-B-5.0.0.0
 *
 * 协议：
 *   App 广播 "LINGOS-DISCOVER" 到 255.255.255.255:2937 (UDP)
 *   本服务响应 JSON: {"type":"lingos","name":...,"version":...,"ip":...,"port":2937,"capabilities":[...]}
 */

#include "discovery_server.h"
#include "../lib/log_extra.h"
#include "../common/safe_string.h"
#include "../lib/cJSON/cJSON.h"
#include "../core/version.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <pthread.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>

#define DISCOVERY_PORT      2937
#define DISCOVERY_MAGIC     "LINGOS-DISCOVER"
#define DISCOVERY_BUF_SIZE  1024

static int discovery_fd = -1;
static pthread_t discovery_thread;
static volatile int discovery_running = 0;

/* 获取本机第一个非回环 IPv4 地址 */
static void discovery_get_local_ip(char *buf, size_t size) {
    struct ifaddrs *ifaddr = NULL;
    if (buf == NULL || size == 0) return;
    safe_strncpy(buf, "0.0.0.0", size);
    if (getifaddrs(&ifaddr) != 0) {
        LOG_WARN_T("Discovery", "GetIP", "GetifaddrsFail", "getifaddrs failed: %s", strerror(errno));
        return;
    }
    for (struct ifaddrs *ifa = ifaddr; ifa != NULL; ifa = ifa->ifa_next) {
        if (ifa->ifa_addr == NULL || ifa->ifa_addr->sa_family != AF_INET) continue;
        if ((ifa->ifa_flags & IFF_LOOPBACK) != 0) continue;
        struct sockaddr_in *sa = (struct sockaddr_in *)ifa->ifa_addr;
        if (inet_ntop(AF_INET, &sa->sin_addr, buf, size) != NULL) break;
    }
    freeifaddrs(ifaddr);
}

/* 构造并发送发现响应 */
static void discovery_respond(const struct sockaddr_in *from, socklen_t from_len) {
    if (from == NULL) return;
    char local_ip[64];
    char hostname[128];
    discovery_get_local_ip(local_ip, sizeof(local_ip));
    if (gethostname(hostname, sizeof(hostname)) != 0) {
        safe_strncpy(hostname, "LING-OS", sizeof(hostname));
    }

    cJSON *root = cJSON_CreateObject();
    if (root == NULL) return;
    cJSON_AddStringToObject(root, "type", "lingos");
    cJSON_AddStringToObject(root, "name", hostname);
    cJSON_AddStringToObject(root, "version", version_get());
    cJSON_AddStringToObject(root, "ip", local_ip);
    cJSON_AddNumberToObject(root, "port", DISCOVERY_PORT);
    cJSON *caps = cJSON_AddArrayToObject(root, "capabilities");
    if (caps != NULL) {
        cJSON_AddItemToArray(caps, cJSON_CreateString("chat"));
        cJSON_AddItemToArray(caps, cJSON_CreateString("command"));
        cJSON_AddItemToArray(caps, cJSON_CreateString("ha"));
    }

    char *json = cJSON_PrintUnformatted(root);
    if (json != NULL) {
        ssize_t sent = sendto(discovery_fd, json, strlen(json), 0,
                              (const struct sockaddr *)from, from_len);
        LOG_DEBUG_T("Discovery", "Respond", "Sent", "responded %d bytes to %s:%d",
                    (int)sent, inet_ntoa(from->sin_addr), ntohs(from->sin_port));
        free(json);
    }
    cJSON_Delete(root);
}

/* 发现循环线程 */
static void *discovery_loop(void *arg) {
    (void)arg;
    char buf[DISCOVERY_BUF_SIZE];
    struct sockaddr_in client_addr;
    while (discovery_running) {
        socklen_t addr_len = sizeof(client_addr);
        ssize_t n = recvfrom(discovery_fd, buf, sizeof(buf) - 1, 0,
                             (struct sockaddr *)&client_addr, &addr_len);
        if (n < 0) {
            if (errno == EINTR) continue;
            if (!discovery_running) break;
            LOG_WARN_T("Discovery", "Loop", "RecvError", "recvfrom failed: %s", strerror(errno));
            break;
        }
        buf[n] = '\0';
        if (strncmp(buf, DISCOVERY_MAGIC, strlen(DISCOVERY_MAGIC)) != 0) continue;
        LOG_INFO_T("Discovery", "Loop", "Discover", "discovery request from %s:%d",
                   inet_ntoa(client_addr.sin_addr), ntohs(client_addr.sin_port));
        discovery_respond(&client_addr, addr_len);
    }
    return NULL;
}

int discovery_server_start(void) {
    if (discovery_running) return 0;

    discovery_fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (discovery_fd < 0) {
        LOG_ERROR_T("Discovery", "Start", "SocketFail", "socket() failed: %s", strerror(errno));
        return -1;
    }

    int opt = 1;
    setsockopt(discovery_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(DISCOVERY_PORT);

    if (bind(discovery_fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        LOG_WARN_T("Discovery", "Start", "BindFail", "bind UDP %d failed: %s", DISCOVERY_PORT, strerror(errno));
        close(discovery_fd);
        discovery_fd = -1;
        return -1;
    }

    discovery_running = 1;
    if (pthread_create(&discovery_thread, NULL, discovery_loop, NULL) != 0) {
        LOG_ERROR_T("Discovery", "Start", "ThreadFail", "pthread_create failed: %s", strerror(errno));
        discovery_running = 0;
        close(discovery_fd);
        discovery_fd = -1;
        return -1;
    }

    LOG_INFO_T("Discovery", "Start", "Started", "UDP discovery listening on port %d", DISCOVERY_PORT);
    return 0;
}

void discovery_server_stop(void) {
    if (!discovery_running) return;
    discovery_running = 0;
    if (discovery_fd >= 0) {
        shutdown(discovery_fd, SHUT_RDWR);
        close(discovery_fd);
        discovery_fd = -1;
    }
    pthread_join(discovery_thread, NULL);
    LOG_INFO_T("Discovery", "Stop", "Stopped", "UDP discovery server stopped");
}

int discovery_server_is_running(void) {
    return discovery_running;
}
