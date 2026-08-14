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
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <linux/videodev2.h>
#include <errno.h>

static int g_camera_fd = -1;
static camera_frame_t g_frame;
static unsigned char *g_frame_buffer = NULL;
static int g_recovery_attempts = 0;
static int g_max_recovery_attempts = 3;

/* ============================================================
 * 初始化摄像头
 * ============================================================ */

int camera_init(const vision_config_t *config) {
    LOG_DEBUG_T("Camera", "Init", "Enter", "device=%s", config->device_path);

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