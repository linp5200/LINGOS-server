/**
 * @file    src/lib/api_log.h
 * @brief   API 日志 + 设备修改日志（先生设定 2026-09-19 · P2-B 实施）
 * @version LN-0.7.0
 *
 * 先生设定摘要：
 *   · API 日志：仅 server mode 可查看；记录范围全：HTTP/WS/TCP/webhook/socket/UDP
 *   · 设备修改日志：7 码（ADD/MOD/DEL/MOV/REN/COPY/APPEND）；两条式（请求 + 完成）；
 *     格式与主日志一致；隐私=摘要或长度（敏感时仅记长度）
 *   · 不被清除：防轮转（单文件）/防手动删除（应用层拦截）/防关机（append）
 *   · 防清屏（dev.prevent_clear——shell ^L 改重绘）
 *
 * 存储：
 *   api.log         —— API 日志（server mode 尾随查看）
 *   device_mod.log  —— 设备修改日志（审计——两条式）
 * 两文件均不参与清理线程的自动删除（不被清除）。
 */

#ifndef LIB_API_LOG_H
#define LIB_API_LOG_H

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 记录一条 API 日志
 * @param channel   http / ws / tcp / webhook / socket / udp / py
 * @param direction in（请求）/ out（响应）
 * @param op        操作（URL / 命令 / 事件类型）
 * @param status    HTTP 状态 / 结果码（0=未知）
 * @param dur_ms    耗时毫秒（可 0）
 * @param bytes     负载字节数（可 0）
 * @param summary   摘要（敏感时调用方先脱敏；可 NULL）
 */
void api_log(const char *channel, const char *direction, const char *op,
             int status, long dur_ms, long bytes, const char *summary);

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
