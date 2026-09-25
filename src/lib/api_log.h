/**
 * @file    src/lib/api_log.h
 * @brief   API 日志 + 设备修改日志（先生设定 2026-09-19 · P2-B 实施；v0.7.2 格式升级）
 * @version LN-0.7.2
 *
 * ── 格式（先生 2026-09-25 定稿 · 方案 B 语义映射）──────────────────
 *   时间 | 方法 | 通道:设备 | 状态 | 类别 | req{大小} "摘要" | resp{大小} "摘要"
 *   例：
 *   2026-09-25 09:34:53 | POST | HTTP:192.168.1.5  | 200 | AIChat     | req{84B} "server_mode_stop" | resp{71B} "status:ok"
 *   2026-09-25 09:34:56 | GET  | WS:ws_8_1790256  | 200 | Telemetry  | req{15B} "system_info"      | resp{212B} "cpu_usage=…"
 *
 *   · 方法：HTTP 通道=真实方法；WS/TCP 等=按命令名语义映射（GET/POST/DELETE）
 *   · 类别：按命令名前缀映射（AIChat/ServerMode/Telemetry/FileOp/…）
 *   · 摘要：前 120 字符；token/password/key 等敏感值自动打码（隐私红线）
 *   · 仅 server mode / 开发者模式可查看（api.log 为专属查看渠道）
 */

#ifndef LIB_API_LOG_H
#define LIB_API_LOG_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/** 摘要最大长度（超出截断加 …） */
#define API_SUMMARY_MAX 120

/**
 * @brief 记录一条 API 日志（7 列格式）
 * @param method   上层方法：HTTP 真实方法（"GET"/"POST"/"DELETE"）；非 HTTP 传 NULL
 *                 （内部按 op 语义映射：查询类→GET / 删除类→DELETE / 其余→POST）
 * @param channel  通道："HTTP" / "WS" / "TCP" / "UDP" / "SOCKET" / "PY"
 * @param device   设备标识（HTTP/UDP=客户端 IP；WS=连接 ID 或 dev:<前8位>；内部="-"，可 NULL）
 * @param status   状态：HTTP 真实码；命令类 200=ok / 500=err / 0=仅请求
 * @param op       操作（URL 路径或命令名）——用于类别映射与显示
 * @param req      请求内容（可为 NULL）——自动打码 + 截断
 * @param req_len  请求字节数（0=按 strlen(req) 估算）
 * @param resp     响应内容（可为 NULL）
 * @param resp_len 响应字节数
 */
void api_log(const char *method, const char *channel, const char *device,
             int status, const char *op,
             const char *req, size_t req_len,
             const char *resp, size_t resp_len);

/**
 * @brief 命令/路径 → 业务类别（AIChat / ServerMode / Telemetry / …）
 */
const char *api_log_category(const char *op);

/**
 * @brief 命令 → HTTP 语义方法（GET/POST/DELETE）——方案 B 映射
 * @param http_method 非 NULL 时直接返回（HTTP 通道用真实方法）
 */
const char *api_log_method(const char *op, const char *http_method);

/**
 * @brief 设备修改日志（两条式）
 * @param op         7 码之一：ADD/MOD/DEL/MOV/REN/COPY/APPEND
 * @param target     目标路径（sensitive=1 时忽略——仅记长度）
 * @param req_id     请求 ID（phase=0 时可为 NULL——自动生成并返回于 out_req_id）
 * @param phase      0=请求（记录）, 1=完成（引用 req_id）
 * @param size       字节长度
 * @param sensitive  1=敏感（仅记长度，不记原文——"摘要或长度"）
 * @param result     phase=1 时的结果描述（ok / fail:xxx）
 */
void devmod_log(const char *op, const char *target, const char *req_id,
                int phase, long size, int sensitive, const char *result);

/**
 * @brief 生成设备修改请求 ID（写进 out）
 */
void devmod_new_req_id(char *out, unsigned int out_sz);

/**
 * @brief 日志保护判定（"不被清除"应用层拦截）
 * @param path 目标路径
 * @return 1 = 受保护（拒绝删除）；0 = 正常
 */
int log_path_protected(const char *path);

#ifdef __cplusplus
}
#endif

#endif /* LIB_API_LOG_H */
