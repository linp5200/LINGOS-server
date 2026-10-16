/**
 * @file    src/monitor/monitord.c
 * @brief   监控系统守护进程（monitor）——采集/预览/快照/录像，**给人看**
 * @version LN-0.9.0
 * @par     架构依据（先生 2026-09-05 裁决 · docs/ARCHITECTURE_系统类目.md）：
 *            monitor      = 采集(V4L2/RTSP)/多路/预览/录像/拍照/IR/OSD/存储 —— 独立进程
 *            ai_vision    = 检测/标定/追踪 —— 独立进程，**经帧通道消费 monitor 帧**
 *          二者经"帧通道 + 事件"解耦，不互相内含。
 * @par     本进程职责：相机采集 → 帧通道发布（供 ai_vision 订阅）→ 快照/录像（存储）
 *          预览与叠加渲染在 Python/App 侧（monitor 只提供纯净帧，默认不含检测框）。
 * @par     权限：采集需 CAMERA 权限（fail-closed；白名单播种 visiond/monitor）。
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>
#include <time.h>

#include "monitor_config.h"
#include "camera_input.h"
#include "frame_channel.h"
#include "frame.h"
#include "ir_control.h"      /* 【0.9.0】红外控制（真实 V4L2 ioctl） */
#include "osd_overlay.h"     /* 【0.9.0】OSD 叠加（5x7 点阵真实绘制） */
#include "../lib/log_extra.h"

#define MONITOR_VERSION "LN-0.9.0"

static volatile sig_atomic_t g_stop = 0;

static void on_signal(int sig) {
    (void)sig;
    g_stop = 1;
}

static void install_signals(void) {
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_signal;
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT, &sa, NULL);
    signal(SIGPIPE, SIG_IGN);   /* 帧通道写失败不当场死 */
}

/* 写 pid 文件（供主程序 terminate_children 收尾） */
static void write_pid_file(void) {
    const char *path = "/LINGOS/run/lingos_monitor.pid";
    FILE *fp = fopen(path, "w");
    if (!fp) {
        LOG_WARN_T("Monitor", "Pid", "WriteFail", "cannot write %s", path);
        return;
    }
    fprintf(fp, "%d\n", (int)getpid());
    fclose(fp);
    LOG_INFO_T("Monitor", "Pid", "Written", "%s (pid %d)", path, (int)getpid());
}

static void remove_pid_file(void) {
    unlink("/LINGOS/run/lingos_monitor.pid");
}

int main(int argc, char **argv) {
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            printf("LINGOS monitor daemon %s\n", MONITOR_VERSION);
            printf("用法: lingos_monitor [--config-only] [--help]\n");
            printf("  采集（V4L2/RTSP）→ 帧通道发布（%s）→ 供 ai_vision 订阅\n",
                   FRAME_CHANNEL_DEFAULT_PATH);
            return 0;
        }
    }
    int config_only = 0;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--config-only") == 0) config_only = 1;
    }

    LOG_INFO_T("Monitor", "Main", "Start", "lingos_monitor %s starting", MONITOR_VERSION);
    install_signals();

    /* ① 配置（monitor.conf；config 系统统一读写——monitor 只管采集） */
    monitor_config_t cfg;
    monitor_config_set_defaults(&cfg);
    if (monitor_config_load(&cfg) != 0) {
        LOG_WARN_T("Monitor", "Main", "ConfigDefault", "monitor config not found — using defaults");
    }
    if (monitor_config_ensure_dirs(&cfg) != 0) {
        LOG_WARN_T("Monitor", "Main", "DirFail", "ensure dirs failed (continuing)");
    }
    LOG_INFO_T("Monitor", "Main", "Config", "source=%s %dx%d@%d id=%s channel=%s",
               cfg.camera_source, cfg.width, cfg.height, cfg.fps, cfg.camera_id,
               cfg.frame_channel_path[0] ? cfg.frame_channel_path : FRAME_CHANNEL_DEFAULT_PATH);
    if (config_only) {
        LOG_INFO_T("Monitor", "Main", "ConfigOnly", "config check done, exiting");
        return 0;
    }

    /* ② 采集身份（设备权限闸门用）——默认 monitor；白名单播种含 monitor 后即可采集。
     *    兼容：若白名单只有 visiond 授权，可用 --app-id visiond 覆盖（camera_set_app_id）。 */
    const char *app_id = "monitor";
    for (int i = 1; i < argc - 1; i++) {
        if (strcmp(argv[i], "--app-id") == 0) app_id = argv[i + 1];
    }
    camera_set_app_id(app_id);
    LOG_INFO_T("Monitor", "Main", "Identity", "camera app_id='%s' (fail-closed if no CAMERA perm)", app_id);

    /* ③ 摄像头 */
    if (camera_init(&cfg) != 0) {
        LOG_ERROR_T("Monitor", "Main", "CameraFail",
                    "camera init failed (source=%s) — exiting (monitor requires a camera)", cfg.camera_source);
        return 1;
    }

    /* ③b 红外控制（真实 V4L2 控制面；驱动没有 IR 控制项 → 明确失败，不伪造） */
    {
        int v4l2_fd = camera_get_fd();
        if (ir_control_init(v4l2_fd) == 0) {
            char ctrls[1024];
            if (ir_control_list_controls(ctrls, sizeof(ctrls)) > 0) {
                LOG_DEBUG_T("Monitor", "Main", "IrControls", "enumerated V4L2 controls:\n%s", ctrls);
            }
            int want = IR_MODE_KEEP;
            if (strcmp(cfg.ir_mode, "off") == 0)       want = IR_MODE_OFF;
            else if (strcmp(cfg.ir_mode, "on") == 0)   want = IR_MODE_ON;
            else if (strcmp(cfg.ir_mode, "auto") == 0) want = IR_MODE_AUTO;
            if (want != IR_MODE_KEEP) {
                if (ir_control_set_mode(want) != 0) {
                    LOG_WARN_T("Monitor", "Main", "IrSetFail", "ir_mode=%s not applied: %s",
                               cfg.ir_mode, ir_control_status());
                } else {
                    LOG_INFO_T("Monitor", "Main", "IrSetOK", "ir_mode=%s applied: %s",
                               cfg.ir_mode, ir_control_status());
                }
            } else {
                LOG_INFO_T("Monitor", "Main", "IrKeep", "ir_mode=keep — %s", ir_control_status());
            }
        } else {
            LOG_WARN_T("Monitor", "Main", "IrUnavailable", "IR control unavailable: %s",
                       ir_control_status());
        }
    }

    /* ③c OSD 叠加（时间戳 + 摄像头 ID；仅未压缩帧可绘制——JPEG 诚实跳过） */
    osd_overlay_init(cfg.osd_enabled, cfg.camera_id, cfg.osd_scale);

    /* ④ 帧通道（发布端） */
    const char *ch_path = cfg.frame_channel_path[0] ? cfg.frame_channel_path : FRAME_CHANNEL_DEFAULT_PATH;
    int max_clients = cfg.frame_channel_max_clients > 0 ? cfg.frame_channel_max_clients : FRAME_CHANNEL_MAX_CLIENTS;
    frame_channel_pub_t *pub = frame_channel_pub_open(ch_path, max_clients);
    if (!pub) {
        LOG_ERROR_T("Monitor", "Main", "ChannelFail", "cannot open frame channel %s", ch_path);
        camera_cleanup();
        return 1;
    }
    LOG_INFO_T("Monitor", "Main", "Channel", "frame channel published at %s (max_clients=%d)", ch_path, max_clients);

    write_pid_file();

    /* ⑤ 采集循环（帧通道发布；快照/录像由后续批次接入存储模块） */
    int interval_ms = cfg.fps > 0 ? (1000 / cfg.fps) : 100;
    if (interval_ms < 1) interval_ms = 1;
    unsigned long long frames = 0, publish_fail = 0;
    unsigned long long osd_drawn = 0, osd_skipped = 0;
    time_t last_report = time(NULL);
    /* OSD 私有帧副本：camera_capture 已在返回前把 V4L2 缓冲 QBUF 归还驱动，
     * 直接在其上绘制会与驱动写帧竞争 → 只对私有副本绘制/发布（驱动缓冲只读） */
    unsigned char *osd_buf = NULL;
    int osd_buf_sz = 0;

    while (!g_stop) {
        camera_frame_t frame;
        memset(&frame, 0, sizeof(frame));
        if (camera_capture(&frame) != 0) {
            LOG_WARN_T("Monitor", "Capture", "Fail", "capture failed (continuing)");
            usleep((useconds_t)(interval_ms * 1000));
            continue;
        }
        frame.seq = (uint32_t)(++frames);
        if (frame.camera_id[0] == '\0') {
            strncpy(frame.camera_id, cfg.camera_id[0] ? cfg.camera_id : "cam0",
                    sizeof(frame.camera_id) - 1);
        }
        /* OSD 叠加：先复制到私有缓冲，再真实绘制（返回码区分"已画/格式不支持/关闭"） */
        if (frame.format == FRAME_FMT_YUYV && frame.data && frame.size > 0) {
            if (frame.size > osd_buf_sz) {
                unsigned char *nb = realloc(osd_buf, (size_t)frame.size);
                if (nb) {
                    osd_buf = nb;
                    osd_buf_sz = frame.size;
                } else {
                    LOG_WARN_T("Monitor", "Osd", "CopyAllocFail",
                               "cannot allocate %d bytes for OSD copy — drawing skipped", frame.size);
                }
            }
            if (osd_buf && frame.size <= osd_buf_sz) {
                memcpy(osd_buf, frame.data, (size_t)frame.size);
                frame.data = osd_buf;
            }
        }
        int osd_rc = osd_overlay_apply(&frame);
        if (osd_rc == OSD_OK) osd_drawn++;
        else if (osd_rc != OSD_SKIP_DISABLED) osd_skipped++;

        if (frame_channel_pub_publish(pub, &frame) != 0) {
            publish_fail++;
        }
        /* 统计上报（每 30s）——可观测性，避免刷屏 */
        time_t now = time(NULL);
        if (now - last_report >= 30) {
            frame_channel_stats_t st;
            frame_channel_pub_stats(pub, &st);
            LOG_INFO_T("Monitor", "Stats", "Report",
                       "frames=%llu fail=%llu clients=%d dropped=%llu bytes=%llu "
                       "osd_drawn=%llu osd_skipped=%llu (%s)",
                       frames, publish_fail, st.clients, st.dropped, st.bytes,
                       osd_drawn, osd_skipped, osd_overlay_status());
            last_report = now;
        }
        usleep((useconds_t)(interval_ms * 1000));
    }

    LOG_INFO_T("Monitor", "Main", "Stop", "signal received — shutting down (frames=%llu)", frames);
    free(osd_buf);
    frame_channel_pub_close(pub);
    ir_control_cleanup();   /* 只解绑，不改硬件状态 */
    osd_overlay_cleanup();
    camera_cleanup();
    remove_pid_file();
    LOG_INFO_T("Monitor", "Main", "Bye", "lingos_monitor exited cleanly");
    return 0;
}
