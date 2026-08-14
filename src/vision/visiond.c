/**
 * @file    src/vision/visiond.c
 * @brief   视觉检测独立子进程（lingos_visiond）
 * @version LN-B-5.0.0.0
 * @changes 添加 vision_config_load、vision_config_set_defaults 实现；
 *          fork 启动 yolo_service.py；
 *          检测 Socket 就绪
 * @par     核心协议：防弹编程（独立进程 + 心跳监控）
 */

#include "visiond.h"
#include "camera_input.h"
#include "vision_config.h"
#include "detection_engine.h"
#include "spatial_mapper.h"
#include "tracker.h"
#include "vision_memory.h"
#include "vision_train.h"
#include "../common/error_report.h"
#include "../common/safe_string.h"
#include "../common/data_path.h"
#include "../common/lang.h"
#include "../lib/log_extra.h"
#include "../core/version.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <time.h>
#include <pthread.h>
#include <ctype.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/stat.h>
#include <errno.h>

/* ============================================================
 * 全局状态
 * ============================================================ */

static volatile int g_running = 1;
static volatile int g_heartbeat = 0;
static pthread_t g_heartbeat_thread;
static pthread_t g_detect_thread;
static pthread_t g_yolo_monitor_thread;
static vision_config_t g_config;
static pid_t g_yolo_pid = -1;

#define YOLO_SOCKET_PATH "/LINGOS/run/yolo.sock"
#define YOLO_SCRIPT_PATH "/LINGOS/bin/yolo_service.py"

/* ============================================================
 * 【新增】YOLO 服务管理
 * ============================================================ */

static int wait_for_yolo_socket(int timeout_sec) {
    LOG_DEBUG_T("Visiond", "WaitYOLO", "Enter", "timeout=%d", timeout_sec);

    int waited = 0;
    while (waited < timeout_sec) {
        if (access(YOLO_SOCKET_PATH, F_OK) == 0) {
            /* 尝试连接验证服务是否就绪 */
            int fd = socket(AF_UNIX, SOCK_STREAM, 0);
            if (fd >= 0) {
                struct sockaddr_un addr;
                memset(&addr, 0, sizeof(addr));
                addr.sun_family = AF_UNIX;
                safe_strncpy(addr.sun_path, YOLO_SOCKET_PATH, sizeof(addr.sun_path));
                if (connect(fd, (struct sockaddr*)&addr, sizeof(addr)) == 0) {
                    close(fd);
                    LOG_INFO_T("Visiond", "WaitYOLO", "Ready", "YOLO service ready after %d seconds", waited);
                    return 0;
                }
                close(fd);
            }
        }
        sleep(1);
        waited++;
    }

    LOG_WARN_T("Visiond", "WaitYOLO", "Timeout", "YOLO service not ready after %d seconds", timeout_sec);
    return -1;
}

static int start_yolo_service(void) {
    LOG_INFO_T("Visiond", "StartYOLO", "Enter", "starting YOLO service");

    if (!g_config.enable_yolo) {
        LOG_DEBUG_T("Visiond", "StartYOLO", "Disabled", "YOLO disabled in config");
        return 0;
    }

    /* 检查脚本是否存在 */
    if (access(YOLO_SCRIPT_PATH, X_OK) != 0) {
        LOG_WARN_T("Visiond", "StartYOLO", "ScriptNotFound", "%s not found or not executable", YOLO_SCRIPT_PATH);
        return -1;
    }

    pid_t pid = fork();
    if (pid == -1) {
        LOG_ERROR_T("Visiond", "StartYOLO", "ForkFail", "fork failed: %s (errno=%d)", strerror(errno), errno);
        return -1;
    }

    if (pid == 0) {
        /* 子进程：执行 YOLO 服务 */
        setsid();
        execlp("python3", "python3", YOLO_SCRIPT_PATH, (char *)NULL);
        _exit(1);
    }

    g_yolo_pid = pid;
    LOG_INFO_T("Visiond", "StartYOLO", "Forked", "YOLO service PID=%d", pid);

    /* 等待 Socket 就绪（最多 30 秒） */
    if (wait_for_yolo_socket(30) != 0) {
        LOG_WARN_T("Visiond", "StartYOLO", "Timeout", "YOLO service may not be ready, continuing anyway");
        /* 不返回错误，允许降级到模拟检测 */
    }

    return 0;
}

static void* yolo_monitor_thread_func(void *arg) {
    (void)arg;
    LOG_DEBUG_T("Visiond", "YOLOMonitor", "Start", "YOLO monitor thread started");

    while (g_running) {
        sleep(30);

        if (!g_config.enable_yolo) {
            continue;
        }

        if (g_yolo_pid > 0) {
            /* 检查 YOLO 进程是否存活 */
            if (kill(g_yolo_pid, 0) != 0) {
                LOG_WARN_T("Visiond", "YOLOMonitor", "Dead", "YOLO service died, restarting...");
                start_yolo_service();
            }
        }

        /* 检查 Socket 是否有效 */
        if (access(YOLO_SOCKET_PATH, F_OK) != 0) {
            LOG_WARN_T("Visiond", "YOLOMonitor", "SocketMissing", "YOLO socket missing, restarting...");
            if (g_yolo_pid > 0) {
                kill(g_yolo_pid, SIGTERM);
                sleep(1);
                kill(g_yolo_pid, SIGKILL);
            }
            start_yolo_service();
        }
    }

    LOG_DEBUG_T("Visiond", "YOLOMonitor", "Stop", "YOLO monitor thread stopped");
    return NULL;
}

/* ============================================================
 * 信号处理
 * ============================================================ */

static void signal_handler(int sig) {
    LOG_INFO_T("Visiond", "Signal", "Received", "signal=%d", sig);
    if (sig == SIGTERM || sig == SIGINT) {
        g_running = 0;
        if (g_yolo_pid > 0) {
            kill(g_yolo_pid, SIGTERM);
        }
    }
}

/* ============================================================
 * 心跳线程
 * ============================================================ */

static void* heartbeat_thread_func(void *arg) {
    (void)arg;
    while (g_running) {
        g_heartbeat = 1;
        sleep(5);
        g_heartbeat = 0;
        sleep(1);
    }
    return NULL;
}

/* ============================================================
 * 检测结果上报 ai_server（0.2.2——App vision_event 广播链）
 * 用 libcurl POST /api/vision_event（Makefile 已链 libcurl）
 * ============================================================ */
#include <curl/curl.h>
#include <cJSON.h>

static void vision_report_results(detection_result_t *results, int count) {
    if (count <= 0) return;
    cJSON *root = cJSON_CreateObject();
    cJSON *arr = cJSON_CreateArray();
    for (int i = 0; i < count && i < 32; i++) {
        cJSON *item = cJSON_CreateObject();
        cJSON_AddStringToObject(item, "label", results[i].label);
        cJSON_AddNumberToObject(item, "confidence", results[i].confidence);
        cJSON_AddNumberToObject(item, "world_x", results[i].world_x);
        cJSON_AddNumberToObject(item, "world_y", results[i].world_y);
        cJSON_AddNumberToObject(item, "track_id", results[i].track_id);
        cJSON_AddItemToArray(arr, item);
    }
    cJSON_AddItemToObject(root, "detections", arr);
    char *json_str = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!json_str) return;

    CURL *curl = curl_easy_init();
    if (curl) {
        struct curl_slist *headers = NULL;
        headers = curl_slist_append(headers, "Content-Type: application/json");
        char url[128];
        safe_snprintf(url, sizeof(url), "http://127.0.0.1:8088/api/vision_event");
        curl_easy_setopt(curl, CURLOPT_URL, url);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, json_str);
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, 2L);
        curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 1L);
        curl_easy_perform(curl);
        curl_easy_cleanup(curl);
        curl_slist_free_all(headers);
    }
    free(json_str);
}

/* ============================================================
 * 检测线程
 * ============================================================ */

static void* detect_thread_func(void *arg) {
    (void)arg;

    if (camera_init(&g_config) != 0) {
        REPORT_ERROR("visiond: camera_init failed");
    }

    if (detection_init(&g_config) != 0) {
        REPORT_ERROR("visiond: detection_init failed");
    }

    spatial_init(&g_config);
    tracker_init();

    while (g_running) {
        camera_frame_t frame;
        if (camera_capture(&frame) != 0) {
            LOG_WARN_T("Visiond", "Detect", "CaptureFail", "camera capture failed");
            sleep(1);
            continue;
        }

        detection_result_t results[32];
        int count = detection_run(&frame, results, 32);

        for (int i = 0; i < count; i++) {
            spatial_map(&results[i]);
        }

        tracker_update(results, count);

        for (int i = 0; i < count; i++) {
            if (results[i].confidence > 0.5) {
                vision_memory_save(&results[i]);
            }
        }

        if (count > 0) {
            LOG_DEBUG_T("Visiond", "Detect", "Results", "detected %d objects", count);
            /* 【0.2.2】上报 ai_server → App vision_event 广播 */
            vision_report_results(results, count);
        }

        usleep(100000);
    }

    camera_cleanup();
    detection_cleanup();
    spatial_cleanup();
    tracker_cleanup();

    return NULL;
}

/* ============================================================
 * 主入口
 * ============================================================ */

int main(int argc, char **argv) {
    (void)argc;
    (void)argv;

    log_system_init();
    LOG_INFO_T("Visiond", "Main", "Start", "LING OS Vision Daemon v%s starting", LINGOS_VERSION);

    signal(SIGTERM, signal_handler);
    signal(SIGINT, signal_handler);

    if (vision_config_load(&g_config) != 0) {
        LOG_WARN_T("Visiond", "Main", "ConfigLoadFail", "using defaults");
        vision_config_set_defaults(&g_config);
    }

    LOG_DEBUG_T("Visiond", "Main", "ConfigStep", "vision config step available");

    /* 【新增】启动 YOLO 服务 */
    start_yolo_service();

    vision_memory_init();

    pthread_create(&g_heartbeat_thread, NULL, heartbeat_thread_func, NULL);
    pthread_create(&g_detect_thread, NULL, detect_thread_func, NULL);
    pthread_create(&g_yolo_monitor_thread, NULL, yolo_monitor_thread_func, NULL);

    pthread_join(g_detect_thread, NULL);

    vision_memory_cleanup();

    if (g_yolo_pid > 0) {
        kill(g_yolo_pid, SIGTERM);
    }

    LOG_INFO_T("Visiond", "Main", "Exit", "visiond exiting");
    return 0;
}