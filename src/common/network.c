/**
 * @file    src/common/network.c
 * @brief   网络检测功能实现
 * @version LN-0.4.3
 */

#include "network.h"
#include "data_path.h"
#include "safe_string.h"
#include "log_extra.h"
#include "lang.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/socket.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <sys/time.h>
#include <signal.h>
#include <time.h>

/* 默认检测目标 */
#define DEFAULT_APT_HOST "archive.ubuntu.com"
#define DEFAULT_PYPI_HOST "pypi.tuna.tsinghua.edu.cn"
#define DEFAULT_TIMEOUT 3

/* 用于超时的 alarm 信号处理 */
static volatile int g_dns_timeout_flag = 0;

static void dns_alarm_handler(int sig) {
    (void)sig;
    g_dns_timeout_flag = 1;
}

static int dns_resolve_with_timeout(const char *host, int timeout) {
    if (!host || !*host) {
        LOG_ERROR_T("Network", "DNS", "InvalidHost", "host is empty");
        return -1;
    }

    /* 设置超时信号 */
    struct sigaction sa, old_sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = dns_alarm_handler;
    sigaction(SIGALRM, &sa, &old_sa);

    g_dns_timeout_flag = 0;
    alarm(timeout > 0 ? timeout : DEFAULT_TIMEOUT);

    struct addrinfo hints, *res;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    int ret = getaddrinfo(host, NULL, &hints, &res);

    alarm(0);  /* 取消超时 */
    sigaction(SIGALRM, &old_sa, NULL);

    if (g_dns_timeout_flag) {
        LOG_WARN_T("Network", "DNS", "Timeout", "DNS lookup for %s timed out after %d s", host, timeout);
        return -1;
    }

    if (ret != 0) {
        LOG_WARN_T("Network", "DNS", "Fail", "getaddrinfo(%s) failed: %s", host, gai_strerror(ret));
        return -1;
    }

    freeaddrinfo(res);
    LOG_DEBUG_T("Network", "DNS", "OK", "DNS lookup for %s succeeded", host);
    return 0;
}

int network_dns_resolve(const char *host, int timeout) {
    if (!host || !*host) host = DEFAULT_APT_HOST;
    if (timeout <= 0) timeout = DEFAULT_TIMEOUT;
    return dns_resolve_with_timeout(host, timeout);
}

int network_check_apt_source(int timeout) {
    LOG_DEBUG_T("Network", "CheckApt", "Enter", "checking apt source reachability");
    int ret = network_dns_resolve(DEFAULT_APT_HOST, timeout);
    if (ret == 0) {
        LOG_INFO_T("Network", "CheckApt", "OK", "apt source %s reachable", DEFAULT_APT_HOST);
    } else {
        LOG_WARN_T("Network", "CheckApt", "Fail", "apt source %s unreachable", DEFAULT_APT_HOST);
    }
    return ret;
}

int network_check_pypi_mirror(int timeout) {
    LOG_DEBUG_T("Network", "CheckPyPI", "Enter", "checking PyPI mirror reachability");
    int ret = network_dns_resolve(DEFAULT_PYPI_HOST, timeout);
    if (ret == 0) {
        LOG_INFO_T("Network", "CheckPyPI", "OK", "PyPI mirror %s reachable", DEFAULT_PYPI_HOST);
    } else {
        LOG_WARN_T("Network", "CheckPyPI", "Fail", "PyPI mirror %s unreachable", DEFAULT_PYPI_HOST);
    }
    return ret;
}

int network_check_online(int timeout) {
    LOG_INFO_T("Network", "CheckOnline", "Start", "performing network pre-check");
    if (timeout <= 0) timeout = DEFAULT_TIMEOUT;

    int dns_ok = network_dns_resolve(DEFAULT_APT_HOST, timeout);
    int apt_ok = 0, pypi_ok = 0;

    if (dns_ok == 0) {
        apt_ok = network_check_apt_source(timeout);
        pypi_ok = network_check_pypi_mirror(timeout);
    } else {
        LOG_WARN_T("Network", "CheckOnline", "DNSFail", "DNS resolution failed, skipping further checks");
        /* 即使 DNS 失败，我们仍认为网络不可用，直接返回 -1 */
        return -1;
    }

    if (apt_ok == 0 && pypi_ok == 0) {
        LOG_INFO_T("Network", "CheckOnline", "OK", "all network checks passed");
        return 0;
    } else {
        LOG_WARN_T("Network", "CheckOnline", "Partial", "network checks failed (apt: %d, pypi: %d)", apt_ok, pypi_ok);
        return -1;
    }
}