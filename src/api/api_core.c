/**
 * @file    src/api/api_core.c
 * @brief   内部 API 框架（注册和调用端点）
 * @version LN-0.4.3
 * @par     核心协议：C1, C-C, AI-CTL
 * @changes 增加所有模块端点注册；
 *          集成 WebSocket 服务器；
 *          增加 http_server 启动。
 */

#include "data_path.h"
#include "../lib/platform.h"
#include "api_core.h"
#include "api_routes.h"
#include "websocket_server.h"
#include "http_server.h"
#include "../common/string_no_sys.h"
#include "log_extra.h"
#include "uart.h"
#include "../core/version.h"
#include "../common/safe_string.h"
#include "../config/config_core.h"
#include "../ai/ai_config.h"
#include "../health/system_health.h"
#include "../lib/port_config.h"
#include <unistd.h>
#include <signal.h>
#include <errno.h>
#include <string.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#define MAX_EP 32

typedef struct { const char *ep; api_handler_t h; } ep_entry_t;
static ep_entry_t reg[MAX_EP];
static int cnt = 0;

/* ============================================================
 * FTF[检测 lingosd 是否已运行（主程序跳过网络服务，避免端口冲突）]
 * ============================================================ */
static int lingosd_alive(void) {
    const char *root = lingos_data_root();
    char pid_path[512];
    safe_snprintf(pid_path, sizeof(pid_path), "%s/run/lingosd.pid", root);
    FILE *fp = fopen(pid_path, "r");
    if (!fp) return 0;
    int pid = 0;
    if (fscanf(fp, "%d", &pid) != 1) {
        fclose(fp);
        return 0;
    }
    fclose(fp);
    if (pid <= 0) return 0;
    /* kill(pid, 0)：仅探测进程是否存在（不发送信号） */
    if (kill(pid, 0) == 0 || errno == EPERM) return 1;
    return 0;
}

/* ============================================================
 * 【0.5.2 修复】端口存活探测（先生 2026-09-12 实测：WS 2939 / HTTP 8080 拒绝访问）
 *   场景：lingosd 进程虽在（PID 检查通过），但其 WS/HTTP 启动失败或线程已死 →
 *         主进程因「lingosd alive」跳过启动 → **无人监听 2939/8080**。
 *   处置：主进程跳过前先实测两个端口；都通 → 正常跳过；
 *         否则告警并**兜底自行启动**（若 lingosd 侧稍后重试，bind 冲突仅打日志，无害）。
 * ============================================================ */
static int port_listening(int port) {
    if (port <= 0) return 0;
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return 0;
    struct sockaddr_in sa;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons((uint16_t)port);
    sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    int ok = (connect(fd, (struct sockaddr *)&sa, sizeof(sa)) == 0);
    close(fd);
    return ok;
}

/* ============================================================
 * 扩展端点前向声明（内部注册使用）
 * ============================================================ */
static int api_handle_system_status(const char *req, char *resp, uint32_t len);
static int api_handle_system_metrics(const char *req, char *resp, uint32_t len);
static int api_handle_ai_status(const char *req, char *resp, uint32_t len);
static int api_handle_skills_list(const char *req, char *resp, uint32_t len);

/* ============================================================
 * API 核心初始化（包含 WebSocket 和 HTTP 服务器）
 * ============================================================ */
void api_core_init(int force_network) {
    cnt = 0;
    for (int i = 0; i < MAX_EP; i++) reg[i].ep = NULL;

    /* 注册核心端点 */
    api_register("status", api_handle_status);
    api_register("perm.list", api_handle_perm_list);
    api_register("fs.info", api_handle_fs_info);

    /* 注册扩展端点（通过内部路由） */
    api_register("system.status", api_handle_system_status);
    api_register("system.metrics", api_handle_system_metrics);
    api_register("ai.status", api_handle_ai_status);
    api_register("skills.list", api_handle_skills_list);

    LOG_INFO_T("API", "Init", "OK", "API framework initialized (7 endpoints)");

    /* 【修复】主程序跳过：若 lingosd 已运行（WS/HTTP 已由其独占），仅注册端点不重复启动 */
    /* 【0.5.2】跳过前实测端口——lingosd 在但端口没起 → 兜底自起（防「无人监听」） */
    if (!force_network && lingosd_alive()) {
        int ws_port = port_config_get(PORT_WS);
        int http_port = port_config_get(PORT_HTTP);
        int ws_ok = port_listening(ws_port);
        int http_ok = port_listening(http_port);
        if (ws_ok && http_ok) {
            LOG_INFO_T("API", "Init", "SkipNetwork",
                       "lingosd running & ports live (ws=%d http=%d), skipping WS/HTTP (main process)",
                       ws_port, http_port);
            return;
        }
        LOG_WARN_T("API", "Init", "FallbackStart",
                   "lingosd alive but ports NOT live (ws=%d:%d http=%d:%d) — starting in main process",
                   ws_port, ws_ok, http_port, http_ok);
    }

    /* 启动 WebSocket 服务器 */
    if (websocket_server_start() != 0) {
        LOG_WARN_T("API", "Init", "WSFail", "WebSocket server failed to start");
    } else {
        LOG_INFO_T("API", "Init", "WSOK", "WebSocket server started on port 2939");
    }

    /* 启动 HTTP 服务器（如果可用） */
    if (http_server_start(8080) != 0) {
        LOG_WARN_T("API", "Init", "HTTPFail", "HTTP server failed to start");
    } else {
        LOG_INFO_T("API", "Init", "HTTPOK", "HTTP server started on port 8080");
    }

    /* 【0.5.2】Ready 消息用实际端口（原硬编码 2939/8080——端口可配后不准） */
    {
        char ready_msg[128];
        safe_snprintf(ready_msg, sizeof(ready_msg),
                      "[API] Ready (7 endpoints, WS on %d, HTTP on %d).\n",
                      port_config_get(PORT_WS), port_config_get(PORT_HTTP));
        uart_puts(ready_msg);
    }
}

/* ============================================================
 * 端点注册
 * ============================================================ */
int api_register(const char *ep, api_handler_t h) {
    if (cnt >= MAX_EP || !ep || !h) return -1;
    reg[cnt].ep = ep;
    reg[cnt].h = h;
    cnt++;
    LOG_DEBUG_T("API", "Register", "OK", "endpoint=%s", ep);
    return 0;
}

/* ============================================================
 * 端点调用
 * ============================================================ */
int api_call(const char *ep, const char *req, char *resp, uint32_t len) {
    if (!ep || !resp) return -1;
    for (int i = 0; i < cnt; i++) {
        if (strcmp(reg[i].ep, ep) == 0) {
            return reg[i].h(req, resp, len);
        }
    }
    LOG_WARN_T("API", "Call", "NotFound", "endpoint=%s", ep);
    return -1;
}

/* ============================================================
 * 核心端点实现
 * ============================================================ */
int api_handle_status(const char *req, char *resp, uint32_t len) {
    (void)req;
    if (len < 64) return -1;
    const char *version = version_get();
    safe_snprintf(resp, len, "{\"status\":\"ok\",\"version\":\"%s\"}", version ? version : "unknown");
    return 0;
}

int api_handle_perm_list(const char *req, char *resp, uint32_t len) {
    (void)req;
    if (len < 128) return -1;
    safe_strncpy(resp, "{\"perms\":\"use 'perm list' command\"}", len);
    return 0;
}

int api_handle_fs_info(const char *req, char *resp, uint32_t len) {
    (void)req;
    if (len < 256) return -1;
    const char *root = lingos_data_root();
    safe_snprintf(resp, len, "{\"root\":\"%s\",\"system_cache\":\"%s/system/cache\",\"user_cache\":\"%s/shared/user/cache\"}",
                  root, root, root);
    return 0;
}

/* ============================================================
 * 扩展端点实现（转发到路由函数）
 * ============================================================ */
static int api_handle_system_status(const char *req, char *resp, uint32_t len) {
    (void)req;
    if (len < 1024) return -1;

    const char *version = version_get();
    const wizard_config_t *cfg = config_core_get();
    int mem = get_memory_usage();
    int disk = get_disk_usage(lingos_data_root());
    double load1, load5, load15;
    get_load_avg(&load1, &load5, &load15);

    safe_snprintf(resp, len,
        "{\"status\":\"ok\",\"version\":\"%s\",\"mode\":\"app\","
        "\"memory_usage\":%d,\"disk_usage\":%d,\"load_avg\":%.2f,"
        "\"ai_backend\":\"%s\",\"language\":\"%s\"}",
        version ? version : "unknown",
        mem, disk, load1,
        cfg && cfg->ai_backend[0] ? cfg->ai_backend : "unknown",
        cfg && cfg->language[0] ? cfg->language : "en"
    );
    return 0;
}

static int api_handle_system_metrics(const char *req, char *resp, uint32_t len) {
    (void)req;
    if (len < 512) return -1;

    int mem = get_memory_usage();
    int disk = get_disk_usage(lingos_data_root());
    double load1, load5, load15;
    get_load_avg(&load1, &load5, &load15);

    safe_snprintf(resp, len,
        "{\"memory_usage\":%d,\"disk_usage\":%d,\"load_avg\":%.2f,\"cpu_cores\":%ld}",
        mem, disk, load1, sysconf(_SC_NPROCESSORS_ONLN)
    );
    return 0;
}

static int api_handle_ai_status(const char *req, char *resp, uint32_t len) {
    (void)req;
    if (len < 256) return -1;

    const ai_config_t *cfg = ai_config_get();
    safe_snprintf(resp, len,
        "{\"available\":%d,\"backend\":\"%s\",\"model\":\"%s\",\"language\":\"%s\"}",
        cfg ? 1 : 0,
        cfg && cfg->backend == AI_BACKEND_OLLAMA ? "ollama" : "deepseek",
        cfg ? cfg->deepseek_model : "unknown",
        cfg ? cfg->language : "en"
    );
    return 0;
}

static int api_handle_skills_list(const char *req, char *resp, uint32_t len) {
    (void)req;
    if (len < 512) return -1;
    safe_strncpy(resp,
        "{\"skills\":[\"file_read\",\"file_write\",\"file_delete\",\"file_list\","
        "\"system_info\",\"system_memory\",\"system_disk\",\"system_cpu\","
        "\"net_ping\",\"net_status\",\"process_list\",\"package_list\","
        "\"memory_read\",\"memory_write\",\"memory_search\"]}",
        len);
    return 0;
}