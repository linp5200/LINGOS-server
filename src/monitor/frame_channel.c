/**
 * @file    src/monitor/frame_channel.c
 * @brief   帧通道实现（发布端 = monitor / 订阅端 = ai_vision）
 * @version LN-0.9.0
 * @par     核心协议：防弹编程——帧通道故障绝不影响 monitor 采集（发布非阻塞）；
 *          ai_vision 侧断连只影响自身（重连即可），不反向拖累 monitor。
 * @par     诚实声明：订阅者落后时**丢帧**并在日志计数，绝不伪造/补帧。
 */

#include "frame_channel.h"
#include "../common/safe_string.h"
#include "../lib/log_extra.h"
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/un.h>
#include <unistd.h>

/* ============================================================
 * 线上固定头（64 字节，多字节字段网络序）
 * ============================================================ */
typedef struct {
    char     magic[4];      /* "LGF1" */
    uint32_t seq;           /* 采集序号 */
    uint32_t width;
    uint32_t height;
    uint32_t format;        /* frame_format_t */
    uint32_t size;          /* 帧数据字节数 */
    uint32_t ts_hi;         /* 时间戳高 32 位 */
    uint32_t ts_lo;         /* 时间戳低 32 位 */
    char     camera_id[32]; /* 摄像头 ID */
} frame_channel_hdr_t;

_Static_assert(sizeof(frame_channel_hdr_t) == 64, "frame channel header must be 64 bytes");

/* ============================================================
 * 发布端
 * ============================================================ */
struct frame_channel_pub {
    int  listen_fd;
    char path[256];
    int  max_clients;
    int  clients[FRAME_CHANNEL_MAX_CLIENTS];
    int  client_count;
    unsigned long long published;
    unsigned long long dropped;
    unsigned long long bytes;
};

/* ============================================================
 * 订阅端
 * ============================================================ */
struct frame_channel_sub {
    int            fd;
    unsigned char *buf;
    int            buf_size;
};

/* ============================================================
 * 工具
 * ============================================================ */

static void set_nonblock(int fd) {
    int fl = fcntl(fd, F_GETFL, 0);
    if (fl >= 0) {
        (void)fcntl(fd, F_SETFL, fl | O_NONBLOCK);
    }
}

/* 非阻塞全量发送：任何 EAGAIN/短写都视为失败（调用方断开该订阅者，不阻塞采集） */
static int send_all_nb(int fd, const void *buf, size_t len) {
    const char *p = (const char *)buf;
    size_t sent = 0;
    while (sent < len) {
        ssize_t r = send(fd, p + sent, len - sent, MSG_NOSIGNAL | MSG_DONTWAIT);
        if (r > 0) {
            sent += (size_t)r;
            continue;
        }
        if (r < 0 && errno == EINTR) continue;
        return -1;   /* EAGAIN / EPIPE / 其他 → 该订阅者不可用 */
    }
    return 0;
}

/* 阻塞全量接收（带 SO_RCVTIMEO）；返回已读字节数（0 = 对端关闭，-1 = 错误） */
static ssize_t recv_all(int fd, void *buf, size_t len) {
    char *p = (char *)buf;
    size_t got = 0;
    while (got < len) {
        ssize_t r = recv(fd, p + got, len - got, 0);
        if (r > 0) {
            got += (size_t)r;
            continue;
        }
        if (r == 0) return (got == 0) ? 0 : -1;
        if (errno == EINTR) continue;
        return -1;
    }
    return (ssize_t)got;
}

/* ============================================================
 * 发布端 API
 * ============================================================ */

frame_channel_pub_t *frame_channel_pub_open(const char *sock_path, int max_clients) {
    const char *path = (sock_path && sock_path[0]) ? sock_path : FRAME_CHANNEL_DEFAULT_PATH;

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    if (strlen(path) >= sizeof(addr.sun_path)) {
        LOG_ERROR_T("FrameChannel", "Open", "PathTooLong", "socket path too long: %s", path);
        return NULL;
    }

    frame_channel_pub_t *pub = calloc(1, sizeof(*pub));
    if (!pub) {
        LOG_ERROR_T("FrameChannel", "Open", "NoMem", "calloc failed");
        return NULL;
    }
    pub->listen_fd = -1;
    for (int i = 0; i < FRAME_CHANNEL_MAX_CLIENTS; i++) pub->clients[i] = -1;
    pub->max_clients = (max_clients > 0 && max_clients < FRAME_CHANNEL_MAX_CLIENTS)
                       ? max_clients : FRAME_CHANNEL_MAX_CLIENTS;
    safe_strncpy(pub->path, path, sizeof(pub->path));

    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        LOG_ERROR_T("FrameChannel", "Open", "SocketFail", "socket failed: %s (errno=%d)",
                    strerror(errno), errno);
        free(pub);
        return NULL;
    }

    (void)unlink(path);   /* 清理上次崩溃残留（本机固定路径，属主即本进程） */

    addr.sun_family = AF_UNIX;
    safe_strncpy(addr.sun_path, path, sizeof(addr.sun_path));

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        LOG_ERROR_T("FrameChannel", "Open", "BindFail", "bind %s failed: %s (errno=%d)",
                    path, strerror(errno), errno);
        close(fd);
        free(pub);
        return NULL;
    }
    if (listen(fd, FRAME_CHANNEL_MAX_CLIENTS) != 0) {
        LOG_ERROR_T("FrameChannel", "Open", "ListenFail", "listen failed: %s", strerror(errno));
        close(fd);
        (void)unlink(path);
        free(pub);
        return NULL;
    }

    /* 帧通道仅本机守护间使用：属主/同组可读写，其他用户无权连接 */
    (void)chmod(path, 0660);
    set_nonblock(fd);

    pub->listen_fd = fd;
    LOG_INFO_T("FrameChannel", "Open", "OK", "frame channel publishing on %s (max_clients=%d)",
               path, pub->max_clients);
    return pub;
}

/* 断开第 idx 个订阅者并从数组中移除 */
static void pub_drop_client(frame_channel_pub_t *pub, int idx) {
    if (!pub || idx < 0 || idx >= pub->client_count) return;
    close(pub->clients[idx]);
    for (int j = idx; j + 1 < pub->client_count; j++) {
        pub->clients[j] = pub->clients[j + 1];
    }
    pub->client_count--;
    pub->clients[pub->client_count] = -1;
    pub->dropped++;
}

/* 接入新订阅者（非阻塞 accept，最多 max_clients 路） */
static void pub_accept_clients(frame_channel_pub_t *pub) {
    if (!pub || pub->listen_fd < 0) return;
    while (pub->client_count < pub->max_clients) {
        int c = accept(pub->listen_fd, NULL, NULL);
        if (c < 0) break;
        set_nonblock(c);
        pub->clients[pub->client_count] = c;
        pub->client_count++;
        LOG_INFO_T("FrameChannel", "Accept", "OK", "subscriber connected (fd=%d, total=%d)",
                   c, pub->client_count);
    }
}

int frame_channel_pub_publish(frame_channel_pub_t *pub, const lingos_frame_t *frame) {
    if (!pub || pub->listen_fd < 0) return -1;
    if (!frame || !frame->data || frame->size <= 0) return -1;
    if (frame->size > FRAME_CHANNEL_MAX_FRAME) {
        LOG_WARN_T("FrameChannel", "Publish", "TooLarge", "frame size %d exceeds limit %d",
                   frame->size, FRAME_CHANNEL_MAX_FRAME);
        return -1;
    }

    pub_accept_clients(pub);

    frame_channel_hdr_t hdr;
    memset(&hdr, 0, sizeof(hdr));
    memcpy(hdr.magic, FRAME_CHANNEL_MAGIC, 4);
    hdr.seq     = htonl(frame->seq);
    hdr.width   = htonl((uint32_t)frame->width);
    hdr.height  = htonl((uint32_t)frame->height);
    hdr.format  = htonl((uint32_t)frame->format);
    hdr.size    = htonl((uint32_t)frame->size);
    {
        uint64_t ts = (uint64_t)frame->timestamp;
        hdr.ts_hi = htonl((uint32_t)(ts >> 32));
        hdr.ts_lo = htonl((uint32_t)(ts & 0xFFFFFFFFu));
    }
    safe_strncpy(hdr.camera_id, frame->camera_id[0] ? frame->camera_id : "cam0",
                 sizeof(hdr.camera_id));

    int delivered = 0;
    for (int i = 0; i < pub->client_count; ) {
        int fd = pub->clients[i];
        if (send_all_nb(fd, &hdr, sizeof(hdr)) != 0 ||
            send_all_nb(fd, frame->data, (size_t)frame->size) != 0) {
            LOG_WARN_T("FrameChannel", "Publish", "DropSlow", "subscriber fd=%d dropped (slow or closed)",
                       fd);
            pub_drop_client(pub, i);
            continue;   /* 数组已左移，idx 不动 */
        }
        delivered++;
        i++;
    }

    if (delivered > 0) {
        pub->published++;
        pub->bytes += (unsigned long long)frame->size;
    }
    return 0;
}

void frame_channel_pub_stats(const frame_channel_pub_t *pub, frame_channel_stats_t *out) {
    if (!out) return;
    memset(out, 0, sizeof(*out));
    if (!pub) return;
    out->clients   = pub->client_count;
    out->published = pub->published;
    out->dropped   = pub->dropped;
    out->bytes     = pub->bytes;
}

void frame_channel_pub_close(frame_channel_pub_t *pub) {
    if (!pub) return;
    for (int i = 0; i < pub->client_count; i++) {
        if (pub->clients[i] >= 0) close(pub->clients[i]);
        pub->clients[i] = -1;
    }
    pub->client_count = 0;
    if (pub->listen_fd >= 0) {
        close(pub->listen_fd);
        pub->listen_fd = -1;
    }
    if (pub->path[0]) (void)unlink(pub->path);
    LOG_INFO_T("FrameChannel", "Close", "OK", "frame channel closed (published=%llu dropped=%llu)",
               pub->published, pub->dropped);
    free(pub);
}

/* ============================================================
 * 订阅端 API
 * ============================================================ */

frame_channel_sub_t *frame_channel_sub_open(const char *sock_path) {
    const char *path = (sock_path && sock_path[0]) ? sock_path : FRAME_CHANNEL_DEFAULT_PATH;

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    if (strlen(path) >= sizeof(addr.sun_path)) {
        LOG_ERROR_T("FrameChannel", "SubOpen", "PathTooLong", "socket path too long: %s", path);
        return NULL;
    }

    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return NULL;

    addr.sun_family = AF_UNIX;
    safe_strncpy(addr.sun_path, path, sizeof(addr.sun_path));

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        LOG_DEBUG_T("FrameChannel", "SubOpen", "ConnectFail", "connect %s failed: %s (errno=%d)",
                    path, strerror(errno), errno);
        close(fd);
        return NULL;
    }

    frame_channel_sub_t *sub = calloc(1, sizeof(*sub));
    if (!sub) {
        close(fd);
        return NULL;
    }
    sub->fd = fd;
    LOG_INFO_T("FrameChannel", "SubOpen", "OK", "subscribed to monitor frame channel %s", path);
    return sub;
}

int frame_channel_sub_recv(frame_channel_sub_t *sub, lingos_frame_t *out, int timeout_ms) {
    if (!sub || sub->fd < 0 || !out) return -1;

    if (timeout_ms >= 0) {
        struct timeval tv;
        tv.tv_sec  = timeout_ms / 1000;
        tv.tv_usec = (timeout_ms % 1000) * 1000;
        (void)setsockopt(sub->fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    }

    frame_channel_hdr_t hdr;
    ssize_t r = recv_all(sub->fd, &hdr, sizeof(hdr));
    if (r == 0) return 1;                      /* 对端关闭 */
    if (r != (ssize_t)sizeof(hdr)) return -1;  /* 超时/半包 */

    if (memcmp(hdr.magic, FRAME_CHANNEL_MAGIC, 4) != 0) {
        LOG_WARN_T("FrameChannel", "SubRecv", "BadMagic", "peer sent incompatible header (protocol mismatch)");
        return -2;
    }

    uint32_t seq    = ntohl(hdr.seq);
    uint32_t width  = ntohl(hdr.width);
    uint32_t height = ntohl(hdr.height);
    uint32_t format = ntohl(hdr.format);
    uint32_t size   = ntohl(hdr.size);

    if (size == 0 || size > FRAME_CHANNEL_MAX_FRAME) {
        LOG_WARN_T("FrameChannel", "SubRecv", "BadSize", "invalid frame size %u", size);
        return -2;
    }

    if (sub->buf_size < (int)size) {
        unsigned char *nb = realloc(sub->buf, size);
        if (!nb) return -1;
        sub->buf = nb;
        sub->buf_size = (int)size;
    }

    if (recv_all(sub->fd, sub->buf, size) != (ssize_t)size) return -1;

    memset(out, 0, sizeof(*out));
    out->data   = sub->buf;
    out->width  = (int)width;
    out->height = (int)height;
    out->size   = (int)size;
    out->format = (frame_format_t)format;
    out->seq    = seq;
    out->timestamp = (time_t)((((uint64_t)ntohl(hdr.ts_hi)) << 32) | (uint64_t)ntohl(hdr.ts_lo));

    /* camera_id 由对端填充（固定 32 字节，不保证 NUL 结尾——逐字节拷并强制收尾） */
    size_t ci = 0;
    for (; ci + 1 < sizeof(out->camera_id) && hdr.camera_id[ci] != '\0'; ci++) {
        out->camera_id[ci] = hdr.camera_id[ci];
    }
    out->camera_id[ci] = '\0';
    if (out->camera_id[0] == '\0') safe_strncpy(out->camera_id, "cam0", sizeof(out->camera_id));

    return 0;
}

int frame_channel_sub_fd(const frame_channel_sub_t *sub) {
    return sub ? sub->fd : -1;
}

void frame_channel_sub_close(frame_channel_sub_t *sub) {
    if (!sub) return;
    if (sub->fd >= 0) close(sub->fd);
    free(sub->buf);
    free(sub);
}
