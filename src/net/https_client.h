/**
 * @file    src/net/https_client.h
 * @brief   轻量 HTTPS 客户端（libssl 直连——生命线数据源专用）
 * @version LN-0.6.2
 *
 * 背景（2026-09-18 接线审计）：
 *   · USGS 地震源已强制 https（http 返回 301）→ alertd 原纯 socket 实现**实际收不到数据**
 *   · wolfx EEW 四源（cenc/sc/cwa/jma）为 https-only
 *   · 本模块用 libssl（soname 在 22.04~25.10 稳定，全捆已收录）补齐 TLS 能力
 *
 * 设计原则（与代码范式对齐）：
 *   · 防弹：URL 解析/域名解析/读写全防错，任何失败返回 NULL 不崩溃
 *   · 诚实：HTTP 状态码如实返回（调用方区分 200/301/错误）
 *   · 生命线优先：verify 参数二段式——调用点先试 verify=1，失败再降级 verify=0+WARN
 *     （"宁可误报不可漏报"——可用性优先，但安全路径优先尝试）
 */

#ifndef NET_HTTPS_CLIENT_H
#define NET_HTTPS_CLIENT_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief HTTPS GET，动态分配响应体（调用方 free）
 * @param url        目标 URL（https:// —— http:// 亦可用，自动明文）
 * @param max_sz     响应体上限（字节——防超大响应打爆内存）
 * @param timeout_s  连接/读写超时（秒）
 * @param verify     1=校验服务器证书（失败即错误）；0=不校验（打 WARN 日志）
 * @param http_code  输出 HTTP 状态码（可为 NULL）
 * @return malloc 的响应体（NUL 结尾）；失败返回 NULL
 */
char *https_get_alloc(const char *url, size_t max_sz, int timeout_s,
                      int verify, int *http_code);

/**
 * @brief HTTPS POST JSON（动态分配响应体；wolfx 等仅需 GET，此接口备用）
 */
char *https_post_json_alloc(const char *url, const char *json, size_t max_sz,
                            int timeout_s, int verify, int *http_code);

#ifdef __cplusplus
}
#endif

#endif /* NET_HTTPS_CLIENT_H */
