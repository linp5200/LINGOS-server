/**
 * @file    src/net/http_client.h
 * @brief   轻量 HTTP 客户端（先生 2026-09-12 要求：链接适配 Ubuntu 22.04~25.10）
 * @version LN-0.5.0
 *
 * ⚠️ 背景（先生实测）：
 *   CI 在 Ubuntu 22.04 编译 → 二进制硬依赖以下 soname：
 *     libldap-2.5.so.0 / liblber-2.5.so.0     ← libcurl 的 LDAP 支持
 *     libavcodec.so.58 / libavformat.so.58     ← librtmp1 ← libcurl 的 RTMP 支持
 *     libswscale.so.5 / libavutil.so.56
 *     libunistring.so.2                        ← libidn2 ← libcurl 的 IDN 支持
 *   先生环境是 Ubuntu 25.10（这些 soname 已变更）→ **二进制起不来**。
 *   审计确认：这 7 个库**全部是 libcurl 的传递依赖**，我们并未直接使用。
 *
 * ✅ 解决方案（本模块）：
 *   · 内网 HTTP POST（alertd / visiond 上报 127.0.0.1:8088）→ **纯 socket 实现**
 *     → 完全不需要 libcurl → 连带消除上述 7 个传递依赖
 *   · 文件下载（可能 https）→ **dlopen 延迟加载 libcurl**
 *     → 有 libcurl 则功能可用；无则优雅降级（不崩溃、不影响启动）
 *
 * 效果：二进制**不再硬依赖 libcurl** → Ubuntu 22.04 ~ 25.10 均可启动。
 */

#ifndef NET_HTTP_CLIENT_H
#define NET_HTTP_CLIENT_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define HTTP_DEFAULT_TIMEOUT 10

/* ============================================================
 * 一、纯 socket HTTP（零外部依赖 —— 用于内网上报）
 * ============================================================ */

/**
 * @brief 发送 HTTP/1.1 请求并读取响应体
 *        （支持 http:// ；https 需 libcurl，纯 socket 分支仅支持 http）
 * @param method    "GET" / "POST" / "PUT" / "DELETE"
 * @param url       目标 URL
 * @param body      请求体（可为 NULL）
 * @param content_type  Content-Type（可为 NULL，默认 application/json）
 * @param resp      响应体输出缓冲
 * @param resp_sz   缓冲大小
 * @param timeout_s 超时秒数
 * @return 0 成功（HTTP 2xx）；-1 网络/解析失败；>0 = HTTP 状态码（非 2xx）
 */
int http_request(const char *method, const char *url,
                 const char *body, const char *content_type,
                 char *resp, size_t resp_sz, int timeout_s);

/**
 * @brief 便捷：POST JSON（内网事件上报主用）
 * @return 0 成功；-1 失败；>0 HTTP 状态码
 */
int http_post_json(const char *url, const char *json,
                   char *resp, size_t resp_sz, int timeout_s);

/**
 * @brief 便捷：GET
 */
int http_get(const char *url, char *resp, size_t resp_sz, int timeout_s);

/* ============================================================
 * 二、文件下载（dlopen libcurl，可选依赖）
 * ============================================================ */

/**
 * @brief 下载 URL 到本地文件
 *        优先使用 dlopen 的 libcurl（支持 https + 断点续传）；
 *        无 libcurl 时回退纯 socket（仅 http://）
 * @return 0 成功；-1 失败（无 curl 且非 http / 网络错误）
 */
int http_download(const char *url, const char *out_path, int timeout_s);

/**
 * @brief libcurl 是否可用（dlopen 结果）
 * @return 1 可用；0 不可用
 */
int http_curl_available(void);

/* ============================================================
 * 三、诊断
 * ============================================================ */
/** 打印一次依赖诊断（是否硬依赖 curl / 哪些运行时库缺失） */
void http_report_capabilities(void);

#ifdef __cplusplus
}
#endif

#endif /* NET_HTTP_CLIENT_H */
