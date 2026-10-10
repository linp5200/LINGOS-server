/**
 * @file    src/monitor/frame_channel.h
 * @brief   帧通道——monitor 发布帧 / ai_vision 订阅帧（二者解耦的唯一数据面）
 * @version LN-0.9.0
 * @par     设计：本机 Unix 流套接字，线上为「64 字节固定头 + 帧数据」；
 *          发布端**非阻塞**发送（慢消费者直接断开，绝不拖慢采集）；
 *          订阅端读到的是**最新帧**语义（网络缓冲排队，落后即丢，符合监控语义）。
 * @par     线协议（LGF1）：
 *          magic[4]="LGF1" | seq | width | height | format | size | ts_hi | ts_lo | camera_id[32]
 *          全部多字节字段为网络字节序；size 为帧数据字节数（紧随头部）。
 */

#ifndef MONITOR_FRAME_CHANNEL_H
#define MONITOR_FRAME_CHANNEL_H

#include "frame.h"

#define FRAME_CHANNEL_DEFAULT_PATH   "/LINGOS/run/frame.sock"
#define FRAME_CHANNEL_MAGIC          "LGF1"
#define FRAME_CHANNEL_MAX_CLIENTS    8
#define FRAME_CHANNEL_MAX_FRAME      (8 * 1024 * 1024)   /* 单帧上限 8MB（4K JPEG 足够） */

typedef struct frame_channel_pub frame_channel_pub_t;
typedef struct frame_channel_sub frame_channel_sub_t;

typedef struct {
    int                clients;     /* 当前订阅者数 */
    unsigned long long published;   /* 成功发布帧数（至少 1 个订阅者收到） */
    unsigned long long dropped;     /* 因慢/断而被丢弃的订阅者次数 */
    unsigned long long bytes;       /* 已发布帧字节数 */
} frame_channel_stats_t;

/* ---------- 发布端（monitor 使用） ---------- */
frame_channel_pub_t *frame_channel_pub_open(const char *sock_path, int max_clients);
int  frame_channel_pub_publish(frame_channel_pub_t *pub, const lingos_frame_t *frame);
void frame_channel_pub_stats(const frame_channel_pub_t *pub, frame_channel_stats_t *out);
void frame_channel_pub_close(frame_channel_pub_t *pub);

/* ---------- 订阅端（ai_vision 使用） ---------- */
frame_channel_sub_t *frame_channel_sub_open(const char *sock_path);
/**
 * 收一帧（阻塞至多 timeout_ms）。
 * @return  0 = 成功（out->data 指向订阅端内部缓冲，下次调用前有效）
 *          1 = 对端关闭（应重连）
 *         -1 = 超时/网络错误（可重试）
 *         -2 = 协议错误（对端不兼容，应重连）
 */
int  frame_channel_sub_recv(frame_channel_sub_t *sub, lingos_frame_t *out, int timeout_ms);
int  frame_channel_sub_fd(const frame_channel_sub_t *sub);
void frame_channel_sub_close(frame_channel_sub_t *sub);

#endif /* MONITOR_FRAME_CHANNEL_H */
