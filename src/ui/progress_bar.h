/**
 * @file    src/ui/progress_bar.h
 * @brief   进度条系统头文件（单行动态刷新）
 * @version LN-0.4.3
 * @par     核心协议：防弹编程
 */

#ifndef UI_PROGRESS_BAR_H
#define UI_PROGRESS_BAR_H

#include <stdint.h>
#include <stddef.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================
 * 进度类型枚举
 * ============================================================ */

typedef enum {
    PROGRESS_TYPE_SYSTEM_PKG,    /* 系统包 (apt/dnf/yum/pacman/zypper/apk) */
    PROGRESS_TYPE_PYTHON_PKG,    /* Python 包 (pip) */
    PROGRESS_TYPE_MODEL,         /* 模型下载 (YOLO/Vosk) */
    PROGRESS_TYPE_FILE_COPY,     /* 文件复制 */
    PROGRESS_TYPE_DIR_CREATE,    /* 目录创建 */
    PROGRESS_TYPE_SERVICE,       /* 服务启动 */
    PROGRESS_TYPE_UNKNOWN        /* 未知类型 */
} progress_type_t;

/* ============================================================
 * 进度上下文结构
 * ============================================================ */

typedef struct {
    char name[64];               /* 当前操作名称 (如 "vosk") */
    progress_type_t type;        /* 操作类型 */
    int progress;                /* 当前进度 0-100 */
    int current_item;            /* 当前第几个 (如 1/10) */
    int total_items;             /* 总个数 (如 10) */
    double speed;                /* 当前速度 (MB/s) */
    double downloaded;           /* 已下载大小 (MB) */
    double total_size;           /* 总大小 (MB) */
    int elapsed_seconds;         /* 已耗时 (秒) */
    int width;                   /* 进度条宽度 (默认 50) */
    int is_complete;             /* 是否完成 (1=完成) */
    int has_error;               /* 是否有错误 (1=有错误) */
    char error_msg[256];         /* 错误信息 */
    time_t start_time;           /* 开始时间 (用于计算耗时) */
    int has_speed;               /* 是否有速度概念 (1=有, 0=无) */
    int last_progress;           /* 上次进度 (用于检测变化) */
    int frame_count;             /* 旋转动画帧计数 */
} progress_ctx_t;

/* ============================================================
 * 核心 API
 * ============================================================ */

/**
 * @brief 初始化进度上下文
 * @param ctx 进度上下文指针
 * @param name 操作名称
 * @param type 操作类型
 * @param total_items 总项目数
 * @param has_speed 是否有速度概念
 * @param width 进度条宽度 (0 表示使用默认 50)
 */
void progress_bar_init(progress_ctx_t *ctx, const char *name,
                       progress_type_t type, int total_items,
                       int has_speed, int width);

/**
 * @brief 更新进度
 * @param ctx 进度上下文指针
 * @param progress 当前进度 (0-100)
 * @param speed 当前速度 (MB/s, 无速度概念时传 0)
 * @param downloaded 已下载大小 (MB, 无速度概念时传 0)
 * @param total_size 总大小 (MB, 无速度概念时传 0)
 */
void progress_bar_update(progress_ctx_t *ctx, int progress,
                         double speed, double downloaded,
                         double total_size);

/**
 * @brief 更新当前项目编号 (用于多项目进度)
 * @param ctx 进度上下文指针
 * @param current_item 当前项目编号 (从 1 开始)
 */
void progress_bar_set_item(progress_ctx_t *ctx, int current_item);

/**
 * @brief 渲染进度条 (单行动态刷新)
 * @param ctx 进度上下文指针
 */
void progress_bar_render(const progress_ctx_t *ctx);

/**
 * @brief 渲染完成状态 (换行显示结果)
 * @param ctx 进度上下文指针
 * @param success 是否成功 (1=成功, 0=失败)
 * @param message 附加消息 (可为 NULL)
 */
void progress_bar_finish(const progress_ctx_t *ctx, int success,
                         const char *message);

/**
 * @brief 获取旋转动画字符
 * @param frame 帧编号 (从 0 开始)
 * @return 当前帧字符
 */
const char* progress_bar_get_spinner_char(int frame);

/**
 * @brief 格式化大小 (自动适配 B/KB/MB/GB)
 * @param bytes 字节数
 * @param buf 输出缓冲区
 * @param size 缓冲区大小
 */
void progress_bar_format_size(double bytes, char *buf, size_t size);

/**
 * @brief 格式化时间 (自动适配 s/m/h)
 * @param seconds 秒数
 * @param buf 输出缓冲区
 * @param size 缓冲区大小
 */
void progress_bar_format_time(int seconds, char *buf, size_t size);

/**
 * @brief 获取终端宽度
 * @return 终端宽度 (字符数)，失败返回 80
 */
int progress_bar_get_terminal_width(void);

#ifdef __cplusplus
}
#endif

#endif /* UI_PROGRESS_BAR_H */