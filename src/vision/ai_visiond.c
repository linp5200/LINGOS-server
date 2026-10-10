/**
 * @file    src/vision/ai_visiond.c
 * @brief   AI 识别引擎守护（ai_vision）——检测/标定/追踪，**给 AI 用**
 * @version LN-0.9.0
 * @par     架构依据（先生 2026-09-05 裁决 · docs/ARCHITECTURE_系统类目.md）：
 *            monitor    采集/预览/快照/录像 —— 给人看，纯净画面（默认无检测框）
 *            ai_vision  检测/标定/追踪     —— 给 AI 用，**经帧通道消费 monitor 帧**
 *          二者经"帧通道 + 事件"解耦，不互相内含；叠加只是"是否把结果画上监控画面"。
 * @par     本进程职责：订阅帧通道 → detection_run → 结果发布（Unix socket，JSON）
 *          Python/技能侧经 ai_vision.latest / ai_vision.status 读取（供 AI 推理与规则触发）。
 * @par     诚实标注：历史 detection_engine 的世界坐标（world_x/world_y）在无标定时恒 0，
 *          标定链（spatial_mapper + calibration 服务）接通后才有真实坐标。
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>
#include <time.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/select.h>
#include <sys/stat.h>   /* chmod：结果 sock 收紧到 0600 */

#include "visiond.h"
#include "vision_config.h"
#include "detection_engine.h"
#include "../monitor/frame_channel.h"
#include "../monitor/frame.h"
#include "../lib/log_extra.h"

#define AI_VISION_VERSION   "LN-0.9.0"
#define AI_VISION_SOCK      "/LINGOS/run/ai_vision.sock"
#define MAX_RESULTS         32

static volatile sig_atomic_t g_stop = 0;
static void on_signal(int sig) { (void)sig; g_stop = 1; }

static void install_signals(void) {
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_signal;
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT, &sa, NULL);
    signal(SIGPIPE, SIG_IGN);
}

/* ---------- 结果面（AI/技能侧读取） ---------- */
static detection_result_t g_last[MAX_RESULTS];
static int      g_last_count = 0;
static uint32_t g_last_seq = 0;
static time_t   g_last_ts = 0;
static unsigned long long g_frames = 0, g_detections = 0;

static int  g_listen_fd = -1;

static int listen_open(void) {
    unlink(AI_VISION_SOCK);
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, AI_VISION_SOCK, sizeof(addr.sun_path) - 1);
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        LOG_ERROR_T("AiVision", "Listen", "BindFail", "bind %s: %s", AI_VISION_SOCK, strerror(errno));
        close(fd);
        return -1;
    }
    chmod(AI_VISION_SOCK, 0600);   /* 【S】仅本机可读——识别结果含家庭画面语义 */
    if (listen(fd, 4) != 0) { close(fd); return -1; }
    LOG_INFO_T("AiVision", "Listen", "OK", "result socket at %s", AI_VISION_SOCK);
    return fd;
}

static void json_escape(const char *src, char *dst, size_t n) {
    size_t j = 0;
    for (size_t i = 0; src && src[i] && j + 2 < n; i++) {
        char c = src[i];
        if (c == '"' || c == '\\') { dst[j++] = '\\'; dst[j++] = c; }
        else if ((unsigned char)c < 0x20) { dst[j++] = ' '; }
        else dst[j++] = c;
    }
    dst[j] = '\0';
}

static void handle_client(int fd) {
    char req[256] = {0};
    ssize_t n = read(fd, req, sizeof(req) - 1);
    if (n <= 0) { close(fd); return; }
    for (ssize_t i = 0; i < n; i++) if (req[i] == '\n') { req[i] = '\0'; break; }
    /* 去掉首尾空白 */
    char *cmd = req;
    while (*cmd == ' ') cmd++;

    char out[4096];
    int off = 0;
    if (strncmp(cmd, "status", 6) == 0) {
        off = snprintf(out, sizeof(out),
            "{\"status\":\"ok\",\"data\":{\"frames_seen\":%llu,\"detections_total\":%llu,"
            "\"last_seq\":%u,\"last_ts\":%ld,\"last_count\":%d,\"version\":\"%s\"}}",
            g_frames, g_detections, g_last_seq, (long)g_last_ts, g_last_count, AI_VISION_VERSION);
    } else {
        /* 默认 latest：最近一帧的检测结果（AI 消费面） */
        off = snprintf(out, sizeof(out),
            "{\"status\":\"ok\",\"data\":{\"seq\":%u,\"ts\":%ld,\"count\":%d,\"detections\":[",
            g_last_seq, (long)g_last_ts, g_last_count);
        for (int i = 0; i < g_last_count && off < (int)sizeof(out) - 200; i++) {
            char label[64];
            json_escape(g_last[i].label, label, sizeof(label));
            off += snprintf(out + off, sizeof(out) - off,
                "%s{\"label\":\"%s\",\"class_id\":%d,\"confidence\":%.3f,"
                "\"x\":%d,\"y\":%d,\"w\":%d,\"h\":%d,\"world_x\":%.1f,\"world_y\":%.1f,\"track_id\":%d}",
                i ? "," : "", label, g_last[i].class_id, g_last[i].confidence,
                g_last[i].x, g_last[i].y, g_last[i].width, g_last[i].height,
                g_last[i].world_x, g_last[i].world_y, g_last[i].track_id);
        }
        off += snprintf(out + off, sizeof(out) - off, "]}}");
    }
    (void)!write(fd, out, strlen(out));
    (void)!write(fd, "\n", 1);
    close(fd);
}

static void write_pid_file(void) {
    FILE *fp = fopen("/LINGOS/run/lingos_ai_vision.pid", "w");
    if (fp) { fprintf(fp, "%d\n", (int)getpid()); fclose(fp); }
}

int main(int argc, char **argv) {
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            printf("LINGOS AI vision engine %s\n", AI_VISION_VERSION);
            printf("用法: lingos_ai_vision [--config-only]\n");
            printf("  订阅帧通道 %s → 检测 → 结果面 %s\n",
                   FRAME_CHANNEL_DEFAULT_PATH, AI_VISION_SOCK);
            return 0;
        }
    }
    int config_only = 0;
    for (int i = 1; i < argc; i++) if (strcmp(argv[i], "--config-only") == 0) config_only = 1;

    LOG_INFO_T("AiVision", "Main", "Start", "lingos_ai_vision %s starting", AI_VISION_VERSION);
    install_signals();

    /* ① 视觉配置 + 检测引擎 */
    vision_config_t cfg;
    vision_config_set_defaults(&cfg);
    if (vision_config_load(&cfg) != 0) {
        LOG_WARN_T("AiVision", "Main", "ConfigDefault", "vision config not found — using defaults");
    }
    LOG_INFO_T("AiVision", "Main", "Config", "model=%s yolo=%d tracking=%d spatial=%d conf=%.2f",
               cfg.model_path[0] ? cfg.model_path : "(default)", cfg.enable_yolo,
               cfg.enable_tracking, cfg.enable_spatial_mapping, cfg.confidence_threshold);
    if (config_only) return 0;

    if (detection_init(&cfg) != 0) {
        LOG_ERROR_T("AiVision", "Main", "DetectionInitFail", "detection_init failed — exiting");
        return 1;
    }

    /* ② 结果面 */
    g_listen_fd = listen_open();
    if (g_listen_fd < 0) {
        LOG_ERROR_T("AiVision", "Main", "SocketFail", "cannot open result socket — exiting");
        detection_cleanup();
        return 1;
    }
    write_pid_file();

    /* ③ 订阅帧通道（monitor 发布端）→ 检测 → 更新最新结果 */
    int sub_retry = 0;
    frame_channel_sub_t *sub = NULL;
    time_t last_report = time(NULL);

    while (!g_stop) {
        if (!sub) {
            sub = frame_channel_sub_open(FRAME_CHANNEL_DEFAULT_PATH);
            if (!sub) {
                sub_retry++;
                if (sub_retry % 10 == 1) {
                    LOG_WARN_T("AiVision", "Sub", "Retry",
                               "frame channel %s not available (retry #%d) — monitor may not be running",
                               FRAME_CHANNEL_DEFAULT_PATH, sub_retry);
                }
                /* 等待期间仍可应答结果面请求 */
                fd_set rf;
                FD_ZERO(&rf);
                FD_SET(g_listen_fd, &rf);
                struct timeval tv = {1, 0};
                if (select(g_listen_fd + 1, &rf, NULL, NULL, &tv) > 0) {
                    int c = accept(g_listen_fd, NULL, NULL);
                    if (c >= 0) handle_client(c);
                }
                continue;
            }
            LOG_INFO_T("AiVision", "Sub", "Connected", "subscribed to frame channel %s",
                       FRAME_CHANNEL_DEFAULT_PATH);
            sub_retry = 0;
        }

        fd_set rf;
        FD_ZERO(&rf);
        FD_SET(g_listen_fd, &rf);
        if (sub) FD_SET(frame_channel_sub_fd(sub), &rf);
        int maxfd = g_listen_fd;
        if (sub && frame_channel_sub_fd(sub) > maxfd) maxfd = frame_channel_sub_fd(sub);
        struct timeval tv = {1, 0};
        int sel = select(maxfd + 1, &rf, NULL, NULL, &tv);
        if (sel < 0 && errno != EINTR) break;

        /* 结果面请求 */
        if (sel > 0 && FD_ISSET(g_listen_fd, &rf)) {
            int c = accept(g_listen_fd, NULL, NULL);
            if (c >= 0) handle_client(c);
        }

        /* 帧 → 检测 */
        if (sub && sel > 0 && FD_ISSET(frame_channel_sub_fd(sub), &rf)) {
            lingos_frame_t frame;
            memset(&frame, 0, sizeof(frame));
            int rc = frame_channel_sub_recv(sub, &frame, 200);
            if (rc == 0) {
                g_frames++;
                detection_result_t results[MAX_RESULTS];
                memset(results, 0, sizeof(results));
                int n = detection_run((const camera_frame_t *)&frame, results, MAX_RESULTS);
                if (n < 0) n = 0;
                if (n > MAX_RESULTS) n = MAX_RESULTS;
                g_last_count = n;
                g_last_seq = frame.seq;
                g_last_ts = frame.timestamp ? frame.timestamp : time(NULL);
                if (n > 0) {
                    memcpy(g_last, results, sizeof(detection_result_t) * (size_t)n);
                    g_detections += (unsigned long long)n;
                }
            } else if (rc == 1 || rc == -2) {
                LOG_WARN_T("AiVision", "Sub", "Reconnect", "frame channel closed/protocol error (rc=%d)", rc);
                frame_channel_sub_close(sub);
                sub = NULL;
            }
        }

        time_t now = time(NULL);
        if (now - last_report >= 30) {
            LOG_INFO_T("AiVision", "Stats", "Report", "frames=%llu detections=%llu last_count=%d",
                       g_frames, g_detections, g_last_count);
            last_report = now;
        }
    }

    LOG_INFO_T("AiVision", "Main", "Stop", "signal received (frames=%llu detections=%llu)",
               g_frames, g_detections);
    if (sub) frame_channel_sub_close(sub);
    if (g_listen_fd >= 0) close(g_listen_fd);
    unlink(AI_VISION_SOCK);
    unlink("/LINGOS/run/lingos_ai_vision.pid");
    detection_cleanup();
    LOG_INFO_T("AiVision", "Main", "Bye", "lingos_ai_vision exited cleanly");
    return 0;
}
