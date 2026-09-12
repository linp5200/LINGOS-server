/**
 * @file    src/net/mhd_compat.h
 * @brief   内置 HTTP 服务器 —— libmicrohttpd 兼容层（先生 2026-09-12）
 * @version LN-0.5.0
 *
 * ⚠️ 为什么需要它（链接适配 Ubuntu 22.04~25.10）：
 *   Ubuntu 22.04: libmicrohttpd12 → libgnutls30 → libidn2-0 → **libunistring.so.2**
 *   Ubuntu 25.10: libmicrohttpd12t64 → libgnutls30t64 → libidn2-0 → **libunistring.so.5**
 *   → 只差一个 soname，22.04 编译的二进制在 25.10 上**无法启动**
 *     （先生实测：`libunistring.so.2: cannot open shared object file`）
 *
 * ✅ 本层提供 **libmicrohttpd 的最小兼容实现**（raw socket + pthread）：
 *   · 覆盖 http_server.c 实际用到的全部 API（116 处调用，API 面仅 ~30 个符号）
 *   · **http_server.c 无需改动**（仅换 include）
 *   · 彻底移除 libmicrohttpd → 连带消除 gnutls/idn2/unistring 依赖链
 *
 * 语义对齐：沿用 MHD 的三段式上传回调（分配 → 消费 → 完成）。
 */

#ifndef NET_MHD_COMPAT_H
#define NET_MHD_COMPAT_H

#include <stddef.h>
#include <stdint.h>
#include <sys/socket.h>
#include <netinet/in.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================
 * 基础类型
 * ============================================================ */
enum MHD_Result { MHD_NO = 0, MHD_YES = 1 };
typedef enum MHD_Result MHD_Result;

typedef struct MHD_Daemon    MHD_Daemon;
typedef struct MHD_Connection MHD_Connection;
typedef struct MHD_Response  MHD_Response;

/* 响应体内存模式 */
enum MHD_ResponseMemoryMode {
    MHD_RESPMEM_PERSISTENT = 0,   /* 调用方保持有效，不释放 */
    MHD_RESPMEM_MUST_FREE,        /* 由本层 free() */
    MHD_RESPMEM_MUST_COPY         /* 本层复制，调用方可立即释放 */
};

/* 取值类型 */
enum MHD_ValueKind {
    MHD_HEADER_KIND = 0,
    MHD_GET_ARGUMENT_KIND,
    MHD_COOKIE_KIND,
    MHD_RESPONSE_HEADER_KIND
};

/* daemon 创建标志（兼容占位——本实现始终用内部线程） */
#define MHD_USE_AUTO                     0x00000000u
#define MHD_USE_INTERNAL_POLLING_THREAD  0x00000000u
#define MHD_USE_THREAD_PER_CONNECTION    0x00000000u

/* 选项（仅 END 会被读取；其余忽略） */
enum MHD_OPTION {
    MHD_OPTION_END = 0,
    MHD_OPTION_NOTIFY_COMPLETED,
    MHD_OPTION_CONNECTION_TIMEOUT
};

/* 连接信息 */
enum MHD_ConnectionInfoType {
    MHD_CONNECTION_INFO_CLIENT_ADDRESS = 0,
    MHD_CONNECTION_INFO_SOCKET_CONTEXT
};

union MHD_ConnectionInfo {
    const struct sockaddr *client_addr;
    void                  *socket_context;
};

/* ============================================================
 * HTTP 状态码（覆盖 http_server.c 所用到者）
 * ============================================================ */
#define MHD_HTTP_OK                    200
#define MHD_HTTP_NO_CONTENT            204
#define MHD_HTTP_MOVED_PERMANENTLY     301
#define MHD_HTTP_FOUND                 302
#define MHD_HTTP_NOT_MODIFIED          304
#define MHD_HTTP_BAD_REQUEST           400
#define MHD_HTTP_UNAUTHORIZED          401
#define MHD_HTTP_FORBIDDEN             403
#define MHD_HTTP_NOT_FOUND             404
#define MHD_HTTP_METHOD_NOT_ALLOWED    405
#define MHD_HTTP_REQUEST_ENTITY_TOO_LARGE 413
#define MHD_HTTP_TOO_MANY_REQUESTS     429
#define MHD_HTTP_INTERNAL_SERVER_ERROR 500
#define MHD_HTTP_NOT_IMPLEMENTED       501
#define MHD_HTTP_SERVICE_UNAVAILABLE   503
#define MHD_HTTP_GATEWAY_TIMEOUT       504

/* ============================================================
 * 回调类型
 * ============================================================ */
typedef MHD_Result (*MHD_AccessHandlerCallback)(void *cls,
                                                MHD_Connection *connection,
                                                const char *url,
                                                const char *method,
                                                const char *version,
                                                const char *upload_data,
                                                size_t *upload_data_size,
                                                void **con_cls);

typedef MHD_Result (*MHD_RequestCallback)(void *cls,
                                          MHD_Connection *connection,
                                          const char *url,
                                          const char *method,
                                          const char *version,
                                          const char *upload_data,
                                          size_t *upload_data_size,
                                          void **con_cls);

/* 占位枚举（供兼容签名） */
enum MHD_RequestTerminationCode { MHD_REQUEST_TERMINATED_COMPLETED_OK = 0 };

typedef void (*MHD_RequestCompletedCallback)(void *cls,
                                             MHD_Connection *connection,
                                             void **con_cls,
                                             enum MHD_RequestTerminationCode toe);

/* ============================================================
 * 服务端 API
 * ============================================================ */

/**
 * @brief 启动 HTTP 服务（兼容 MHD_start_daemon 签名）
 * @param flags          忽略（始终内部线程 + 每连接线程）
 * @param port           监听端口
 * @param apc            访问回调（可为 NULL，本实现不调用）
 * @param apc_cls
 * @param rcb            请求回调（**必需**）
 * @param rcb_cls        回调上下文
 * @param ...            选项列表，以 MHD_OPTION_END 结束
 * @return Daemon 句柄；NULL 表示失败
 */
MHD_Daemon *MHD_start_daemon(unsigned int flags, unsigned short port,
                             MHD_AccessHandlerCallback apc, void *apc_cls,
                             MHD_RequestCallback rcb, void *rcb_cls, ...);

void MHD_stop_daemon(MHD_Daemon *daemon);

/* ============================================================
 * 请求侧
 * ============================================================ */

/** 查询 header / GET 参数（返回 NULL 表示不存在） */
const char *MHD_lookup_connection_value(MHD_Connection *connection,
                                        enum MHD_ValueKind kind,
                                        const char *key);

/** 连接信息 */
const union MHD_ConnectionInfo *MHD_get_connection_info(MHD_Connection *connection,
                                                        enum MHD_ConnectionInfoType info_type);

/* ============================================================
 * 响应侧
 * ============================================================ */

MHD_Response *MHD_create_response_from_buffer(size_t size, void *buffer,
                                              enum MHD_ResponseMemoryMode mode);

/** 便捷：从静态字符串创建（不复制、不释放） */
MHD_Response *MHD_create_response_from_data(size_t size, void *data,
                                            int must_free, int must_copy);

MHD_Result MHD_add_response_header(MHD_Response *response,
                                   const char *header, const char *content);

MHD_Result MHD_add_response_footer(MHD_Response *response,
                                   const char *footer, const char *content);

MHD_Result MHD_queue_response(MHD_Connection *connection,
                              unsigned int status_code,
                              MHD_Response *response);

void MHD_destroy_response(MHD_Response *response);

#ifdef __cplusplus
}
#endif

#endif /* NET_MHD_COMPAT_H */
