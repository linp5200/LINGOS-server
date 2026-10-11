/**
 * @file    security_wire.h
 * @brief   安全模块接线中枢（第5期·死代码接线上岗）
 * @version LN-0.9.0-p5
 *
 * 背景（先生铁律：完整实现 ≠ 上岗，交付前必查「谁调用它」）：
 *   以下模块此前**全仓零调用者**，本中枢把它们接进真实调用路径：
 *     · input_filter.c     提示注入 / 危险命令模式过滤
 *     · output_filter.c    输出脱敏（IP / 邮箱）
 *     · secure_memory.c    锁定内存（口令、密钥、脱敏工作区）
 *     · firewall.c         防火墙规则（拒绝即封堵，含目标校验）
 *     · verify/second_verify.c  高危操作二次验证
 *     · shadow_mode.c      影子模式空数据（依赖 defense_mode 状态机）
 *     · dark_mode.c        暗影模式（功能封锁 / 硬件模拟关闭）
 *     · virus_scanner.c    安装包病毒扫描
 *
 * 接线关系（谁调用它）：
 *   security_wire_init()  ← src/daemon/lingosd.c（Lead 集成）
 *   security_wire_check_input()  ← 各输入面（AI/API/MQTT/shell）
 *   security_wire_redact_output() ← 对外输出面（日志 / API 响应）
 *   security_wire_shadow_query()  ← App 数据读取面（影子模式返回空数据）
 *   security_wire_command_allowed() / security_wire_confirm_high_risk()
 *                                ← 命令执行面（高危二次验证）
 *
 * 自检：security_wire_init() 会跑 security_wire_self_test()，
 *       任一模模块接线失效（被移出 Makefile / 语义被改坏）都会返回失败并打 ERROR。
 */

#ifndef SECURITY_SECURITY_WIRE_H
#define SECURITY_SECURITY_WIRE_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================
 * 决策返回值（check_input / command_allowed 共用）
 * ============================================================ */
#define SECURITY_WIRE_ALLOW        0   /* 放行 */
#define SECURITY_WIRE_SHADOW       1   /* 影子模式：调用方应返回空/假成功数据 */
#define SECURITY_WIRE_NEED_CONFIRM 2   /* 需二次确认（调用 security_wire_confirm_high_risk） */
#define SECURITY_WIRE_DENY        (-1) /* 拒绝 */

/**
 * @brief 安全中枢初始化（幂等）：装载过滤规则、编译脱敏正则、初始化二次验证，并自检
 * @return 0 全部就绪；-1 有模块自检失败或初始化失败（调用方应记录但不必中止系统）
 */
int security_wire_init(void);

/**
 * @brief 接线自检：逐模块验证「能被调用且语义正确」
 * @param report 输出报告（可 NULL）
 * @param report_len 报告缓冲大小
 * @return 失败项数（0 = 全部通过），-1 参数错误
 */
int security_wire_self_test(char *report, size_t report_len);

/**
 * @brief 输入面统一审查（执行门 + 注入过滤 + 影子模式）
 * @param input  待审查输入
 * @param source 来源标签："shell"/"ai"/"ai_tool"/"mqtt"/"api"/"file"/"plugin"...
 * @param reason 拒绝原因输出（可 NULL）
 * @param reason_len 原因缓冲大小
 * @return SECURITY_WIRE_ALLOW / SHADOW / NEED_CONFIRM / DENY
 */
int security_wire_check_input(const char *input, const char *source,
                              char *reason, size_t reason_len);

/**
 * @brief 命令执行面审查（暗影模式功能黑名单 + 执行门 + 高危二次验证）
 * @return SECURITY_WIRE_ALLOW / NEED_CONFIRM / DENY
 */
int security_wire_command_allowed(const char *cmdline, char *reason, size_t reason_len);

/**
 * @brief 高危操作二次验证（verify/second_verify.c）
 *        交互终端 → 密码 + Y/N；非交互 → 未设密码放行（审计）、已设密码 fail-closed 拒绝
 * @return 1 批准，0 拒绝
 */
int security_wire_confirm_high_risk(const char *operation);

/**
 * @brief 输出面脱敏（output_filter.c）：IP / 邮箱打码；影子模式下返回空串
 * @return 内部缓冲指针（勿 free；下次调用会被覆盖）
 */
const char *security_wire_redact_output(const char *output);

/**
 * @brief 影子模式数据闸门（shadow_mode.c）
 * @param app_id 应用 id
 * @param perm   权限/资源标识
 * @param type   期望的空数据类型："string"/"list"/"object"/"number"
 * @param out    输出缓冲
 * @param out_len 缓冲大小
 * @return 1 = 影子模式返回空数据（out 已填）；0 = 正常返回；-1 参数错误
 */
int security_wire_shadow_query(const char *app_id, const char *perm,
                               const char *type, char *out, size_t out_len);

/**
 * @brief 防火墙封堵（firewall.c）：目标先过 firewall_is_valid_target 校验
 * @return 0 成功，-1 失败/参数非法
 */
int security_wire_firewall_block(const char *host, const char *port);

/**
 * @brief 安装包病毒扫描（virus_scanner.c）
 * @param reason 结果名输出（"Clean"/"Suspicious"/"Malicious"）
 * @return 0 干净，1 可疑，-1 恶意或参数错误
 */
int security_wire_scan_package(const char *path, char *reason, size_t reason_len);

/** @brief 当前防御模式名（none/shadow/dark/absolute） */
const char *security_wire_mode_name(void);

/** @brief 最近一次自检失败项数（未初始化 = -1） */
int security_wire_last_self_test_failures(void);

#ifdef __cplusplus
}
#endif

#endif /* SECURITY_SECURITY_WIRE_H */
