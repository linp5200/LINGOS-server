/**
 * @file    camera_input.c
 * @brief   摄像头输入层
 * @version LN-B-5.0.0.0
 * @par     核心协议：防御性编程 + 容错编程
 * @changes V4L2 恢复机制增强；安全字符串替换
 */

#include "camera_input.h"
#include "../common/error_report.h"
#include "../common/safe_string.h"
#include "../common/lang.h"
#include "../lib/log_extra.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <linux/videodev2.h>
#include <errno.h>

static int g_camera_fd = -1;
/* 【0.2.2】RTSP 模式（camera_source=rtsp——Python 拉流，先生裁决） */
static int g_rtsp_mode = 0;
static int g_rtsp_sock = -1;
static char g_rtsp_url[256] = {0};
static int g_rtsp_frame_port = 8890;
static int g_rtsp_http_port = 8891;
static camera_frame_t g_frame;
static unsigned char *g_frame_buffer = NULL;
static int g_frame_buffer_size = 0;
static int g_recovery_attempts = 0;
static int g_max_recovery_attempts = 3;

/* ============================================================
 * 初始化摄像头
 * ============================================================ */

int camera_init(const vision_config_t *config) {
    LOG_DEBUG_T("Camera", "Init", "Enter", "device=%s source=%s", config->device_path,
                config->camera_source[0] ? config->camera_source : "v4l2");

    /* 【0.2.2】RTSP 模式：启动 Python 拉流服务 + 连接帧通道（先生裁决：RTSP 走 Python） */
    if (config->camera_source[0] && strcmp(config->camera_source, "v4l2") != 0) {
        g_rtsp_mode = 1;
        safe_strncpy(g_rtsp_url, config->rtsp_url, sizeof(g_rtsp_url));
        g_rtsp_frame_port = config->rtsp_frame_port > 0 ? config->rtsp_frame_port : 8890;
        g_rtsp_http_port = config->rtsp_http_port > 0 ? config->rtsp_http_port : 8891;
        if (g_rtsp_url[0] == '\0') {
            LOG_ERROR_T("Camera", "Init", "RTSPNoUrl", "camera_source=rtsp 但未配置 rtsp_url");
            return -1;
        }
        /* 启动 rtsp_streamer.py（Python 拉流——ffmpeg） */
        char cmd[512];
        const char *py = "/LINGOS/python/bin/python3";
        if (access(py, X_OK) != 0) py = "python3";
        safe_snprintf(cmd, sizeof(cmd),
                      "%s /LINGOS/bin/rtsp_streamer.py --url \"%s\" --frame-port %d --http-port %d >/dev/null 2>&1 &",
                      py, g_rtsp_url, g_rtsp_frame_port, g_rtsp_http_port);
        system(cmd);
        LOG_INFO_T("Camera", "Init", "RTSPStart", "rtsp_streamer started: %s", g_rtsp_url);
        /* 等待流服务就绪后连接帧通道 */
        usleep(1500000);
        g_rtsp_sock = socket(AF_INET, SOCK_STREAM, 0);
        struct sockaddr_in addr;
        memset(&addr, 0, sizeof(addr));
        addr.sin_family = AF_INET;
        addr.sin_port = htons(g_rtsp_frame_port);
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (connect(g_rtsp_sock, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
            LOG_WARN_T("Camera", "Init", "RTSPConnFail", "无法连接帧通道 %d——重试中", g_rtsp_frame_port);
            close(g_rtsp_sock);
            g_rtsp_sock = -1;
            /* 非致命——capture 时重连 */
        }
        return 0;
    }

    const char *device = config->device_path[0] ? config->device_path : "/dev/video0";
    g_camera_fd = open(device, O_RDWR);
    if (g_camera_fd < 0) {
        LOG_ERROR_T("Camera", "Init", "OpenFail", "cannot open %s: %s (errno=%d)",
                    device, strerror(errno), errno);
        /* 尝试使用默认设备 */
        g_camera_fd = open("/dev/video0", O_RDWR);
        if (g_camera_fd < 0) {
            LOG_ERROR_T("Camera", "Init", "OpenFail", "cannot open /dev/video0: %s", strerror(errno));
            return -1;
        }
        LOG_WARN_T("Camera", "Init", "Fallback", "using /dev/video0");
    }

    struct v4l2_capability cap;
    if (ioctl(g_camera_fd, VIDIOC_QUERYCAP, &cap) < 0) {
        LOG_ERROR_T("Camera", "Init", "QueryCapFail", "VIDIOC_QUERYCAP failed: %s", strerror(errno));
        close(g_camera_fd);
        g_camera_fd = -1;
        return -1;
    }

    LOG_DEBUG_T("Camera", "Init", "Cap", "driver=%s, card=%s, bus=%s",
                cap.driver, cap.card, cap.bus_info);

    struct v4l2_format fmt;
    memset(&fmt, 0, sizeof(fmt));
    fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    fmt.fmt.pix.width = config->width > 0 ? config->width : 640;
    fmt.fmt.pix.height = config->height > 0 ? config->height : 480;
    fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_YUYV;
    fmt.fmt.pix.field = V4L2_FIELD_NONE;

    if (ioctl(g_camera_fd, VIDIOC_S_FMT, &fmt) < 0) {
        LOG_WARN_T("Camera", "Init", "SetFormatFail", "VIDIOC_S_FMT failed: %s, using default", strerror(errno));
    }

    struct v4l2_requestbuffers req;
    memset(&req, 0, sizeof(req));
    req.count = 2;
    req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    req.memory = V4L2_MEMORY_MMAP;

    if (ioctl(g_camera_fd, VIDIOC_REQBUFS, &req) < 0) {
        LOG_ERROR_T("Camera", "Init", "ReqBufsFail", "VIDIOC_REQBUFS failed: %s", strerror(errno));
        close(g_camera_fd);
        g_camera_fd = -1;
        return -1;
    }

    struct v4l2_buffer buf;
    memset(&buf, 0, sizeof(buf));
    buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    buf.memory = V4L2_MEMORY_MMAP;
    buf.index = 0;

    if (ioctl(g_camera_fd, VIDIOC_QUERYBUF, &buf) < 0) {
        LOG_ERROR_T("Camera", "Init", "QueryBufFail", "VIDIOC_QUERYBUF failed: %s", strerror(errno));
        close(g_camera_fd);
        g_camera_fd = -1;
        return -1;
    }

    g_frame_buffer = (unsigned char*)mmap(NULL, buf.length, PROT_READ | PROT_WRITE,
                                           MAP_SHARED, g_camera_fd, buf.m.offset);
    if (g_frame_buffer == MAP_FAILED) {
        LOG_ERROR_T("Camera", "Init", "MmapFail", "mmap failed: %s", strerror(errno));
        close(g_camera_fd);
        g_camera_fd = -1;
        return -1;
    }

    g_frame.width = fmt.fmt.pix.width;
    g_frame.height = fmt.fmt.pix.height;
    g_frame.data = g_frame_buffer;
    g_frame.size = buf.length;

    enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (ioctl(g_camera_fd, VIDIOC_STREAMON, &type) < 0) {
        LOG_ERROR_T("Camera", "Init", "StreamOnFail", "VIDIOC_STREAMON failed: %s", strerror(errno));
        munmap(g_frame_buffer, buf.length);
        close(g_camera_fd);
        g_camera_fd = -1;
        return -1;
    }

    g_recovery_attempts = 0;
    LOG_INFO_T("Camera", "Init", "OK", "camera initialized: %dx%d", g_frame.width, g_frame.height);
    return 0;
}

/* ============================================================
 * 【修改】捕获帧（含恢复机制）
 * ============================================================ */

int camera_capture(camera_frame_t *frame) {
    /* 【0.2.2】RTSP 模式：从帧通道读 JPEG 帧（Python 拉流） */
    if (g_rtsp_mode) {
        if (g_rtsp_sock < 0) {
            /* 重连帧通道 */
            g_rtsp_sock = socket(AF_INET, SOCK_STREAM, 0);
            struct sockaddr_in addr;
            memset(&addr, 0, sizeof(addr));
            addr.sin_family = AF_INET;
            addr.sin_port = htons(g_rtsp_frame_port);
            addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
            if (connect(g_rtsp_sock, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
                close(g_rtsp_sock);
                g_rtsp_sock = -1;
                usleep(500000);
                return -1;
            }
            /* 读握手行 */
            char handshake[32] = {0};
            recv(g_rtsp_sock, handshake, sizeof(handshake), 0);
        }
        /* 发请求 → 收 4 字节长度 + JPEG 帧 */
        if (send(g_rtsp_sock, "GET", 3, 0) <= 0) {
            close(g_rtsp_sock);
            g_rtsp_sock = -1;
            return -1;
        }
        uint32_t len = 0;
        int r = recv(g_rtsp_sock, &len, 4, MSG_WAITALL);
        if (r != 4) {
            close(g_rtsp_sock);
            g_rtsp_sock = -1;
            return -1;
        }
        len = ntohl(len);
        if (len == 0 || len > 2 * 1024 * 1024) {
            return -1;
        }
        /* 复用 g_frame_buffer 存放 JPEG */
        if (g_frame_buffer == NULL || (int)len > g_frame_buffer_size) {
            unsigned char *nb = realloc(g_frame_buffer, len);
            if (!nb) return -1;
            g_frame_buffer = nb;
            g_frame_buffer_size = len;
        }
        int got = recv(g_rtsp_sock, g_frame_buffer, len, MSG_WAITALL);
        if (got != (int)len) {
            close(g_rtsp_sock);
            g_rtsp_sock = -1;
            return -1;
        }
        frame->data = g_frame_buffer;
        frame->width = 640;
        frame->height = 480;  /* rtsp_streamer 固定 scale=640:-1——实际高度由检测端解析 */
        frame->size = (int)len;
        frame->timestamp = time(NULL);
        return 0;
    }

    if (g_camera_fd < 0) {
        LOG_ERROR_T("Camera", "Capture", "NotInit", "camera not initialized");
        return -1;
    }

    struct v4l2_buffer buf;
    memset(&buf, 0, sizeof(buf));
    buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    buf.memory = V4L2_MEMORY_MMAP;

    int ret = ioctl(g_camera_fd, VIDIOC_DQBUF, &buf);
    if (ret < 0) {
        LOG_WARN_T("Camera", "Capture", "DQBufFail", "VIDIOC_DQBUF failed: %s (errno=%d)",
                   strerror(errno), errno);

        /* 恢复机制：尝试重新启动流 */
        if (g_recovery_attempts < g_max_recovery_attempts) {
            g_recovery_attempts++;
            LOG_WARN_T("Camera", "Capture", "Recovery", "attempt %d/%d",
                       g_recovery_attempts, g_max_recovery_attempts);

            enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
            ioctl(g_camera_fd, VIDIOC_STREAMOFF, &type);
            usleep(100000);
            ioctl(g_camera_fd, VIDIOC_STREAMON, &type);
            usleep(100000);
        } else {
            LOG_ERROR_T("Camera", "Capture", "RecoveryFail", "max recovery attempts reached");
            return -1;
        }
        return -1;
    }

    g_recovery_attempts = 0;

    if (frame) {
        frame->width = g_frame.width;
        frame->height = g_frame.height;
        frame->size = g_frame.size;
        frame->data = g_frame_buffer;
        frame->timestamp = time(NULL);
    }

    if (ioctl(g_camera_fd, VIDIOC_QBUF, &buf) < 0) {
        LOG_WARN_T("Camera", "Capture", "QBufFail", "VIDIOC_QBUF failed: %s", strerror(errno));
    }

    return 0;
}

/* ============================================================
 * 清理
 * ============================================================ */

void camera_cleanup(void) {
    /* 【0.2.2】RTSP 模式清理 */
    if (g_rtsp_mode) {
        if (g_rtsp_sock >= 0) {
            close(g_rtsp_sock);
            g_rtsp_sock = -1;
        }
        if (g_frame_buffer) {
            free(g_frame_buffer);
            g_frame_buffer = NULL;
            g_frame_buffer_size = 0;
        }
        g_rtsp_mode = 0;
        LOG_DEBUG_T("Camera", "Cleanup", "RTSP", "rtsp camera cleaned up");
        return;
    }
    if (g_camera_fd >= 0) {
        enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        ioctl(g_camera_fd, VIDIOC_STREAMOFF, &type);
        if (g_frame_buffer) {
            munmap(g_frame_buffer, g_frame.size);
            g_frame_buffer = NULL;
        }
        close(g_camera_fd);
        g_camera_fd = -1;
        LOG_DEBUG_T("Camera", "Cleanup", "OK", "camera cleaned up");
    }
}