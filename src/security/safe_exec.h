/**
 * @file    src/security/safe_exec.h
 * @brief   安全命令执行（OWASP OS Command Injection Defense 落地）
 * @version LN-0.5.0
 *
 * 背景（先生 2026-09-12 裁决 S2 + 我方按 OWASP 补强）：
 *   原实现 syscall_handler.c 用 popen(整句字符串) → 等于把 OWASP 的
 *   ①② 两层防御全踩了，只剩最弱的黑名单。
 *
 * OWASP 官方三层做法：
 *   ① 首选：不调用系统命令（用内置函数）
 *   ② 次选：参数化 —— 命令与参数分开传，不经 shell
 *   ③ 必配：输入验证 —— 命令白名单 + 参数校验
 *   （另有：-- 选项终止符；最小权限）
 *
 * 本模块提供：
 *   safe_exec_check()   —— 策略检查（白名单/危险模式）→ 返回决策
 *   safe_exec_run()     —— 参数化执行（fork + execvp，不经 shell）
 *   safe_exec_split()   —— 命令行拆分为 argv（仅用于兼容旧接口）
 */

#ifndef SAFE_EXEC_H
#define SAFE_EXEC_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================
 * 决策结果
 * ============================================================ */
typedef enum {
    SAFE_EXEC_ALLOW = 0,      /* 允许直接执行 */
    SAFE_EXEC_NEED_CONFIRM,   /* 允许但需二次确认（高风险） */
    SAFE_EXEC_DENY            /* 拒绝（白名单外 / 命中危险模式） */
} safe_exec_verdict_t;

/* 策略模式（可由配置切换） */
typedef enum {
    SAFE_EXEC_MODE_STRICT = 0,   /* 白名单制（推荐，先生默认） */
    SAFE_EXEC_MODE_BALANCED,     /* 白名单 + 危险模式黑名单（默认） */
    SAFE_EXEC_MODE_PERMISSIVE    /* 仅危险模式黑名单（兼容旧行为） */
} safe_exec_mode_t;

/* 最大参数个数 / 单参数长度（防资源耗尽 OWASP LLM10） */
#define SAFE_EXEC_MAX_ARGS   64
#define SAFE_EXEC_MAX_ARGLEN 1024
#define SAFE_EXEC_MAX_OUTPUT (1024 * 1024)   /* 1MB */

/* ============================================================
 * 策略
 * ============================================================ */

/** 设置策略模式（默认 BALANCED） */
void safe_exec_set_mode(safe_exec_mode_t mode);
safe_exec_mode_t safe_exec_get_mode(void);

/**
 * @brief 判定一条命令行是否可执行（白名单 + 危险模式）
 * @param cmdline  原始命令行（会被解析）
 * @param reason   失败原因（可为 NULL）
 * @param reason_sz
 * @return verdict
 */
safe_exec_verdict_t safe_exec_check(const char *cmdline,
                                    char *reason, size_t reason_sz);

/**
 * @brief 检查单一可执行名是否在白名单（供参数化调用直接使用）
 */
int safe_exec_program_allowed(const char *prog);

/* ============================================================
 * 执行
 * ============================================================ */

/**
 * @brief 参数化执行（fork + execvp，**不经 shell**）
 *        OWASP ② —— 命令与参数分离，shell 元字符失效
 * @param argv      argv[0]=程序，NULL 结尾
 * @param out       输出缓冲（stdout+stderr 合并）
 * @param out_sz    缓冲大小
 * @param timeout_s 超时秒数（0=不限制）
 * @return 0 成功；-1 失败；-2 超时；-3 非白名单
 */
int safe_exec_run(char *const argv[], char *out, size_t out_sz, int timeout_s);

/**
 * @brief 命令行 → argv 拆分（**仅用于受控输入**；不等价于参数化安全）
 *        按空白拆分，**不解析引号/管道/重定向** —— 保留了它们的字面形态，
 *        交给 execvp 后它们只是普通参数，不会被 shell 解释。
 * @return 参数个数；-1 失败
 */
int safe_exec_split(const char *cmdline, char *buf, size_t buf_sz,
                    char *argv[], int max_args);

/**
 * @brief 便捷：检查 + 拆分 + 执行（供 exec_command 兼容接口使用）
 * @param need_confirm 出参：是否需要二次确认（可为 NULL）
 */
int safe_exec_run_cmdline(const char *cmdline, char *out, size_t out_sz,
                          int timeout_s, int *need_confirm,
                          char *reason, size_t reason_sz);

#ifdef __cplusplus
}
#endif

#endif /* SAFE_EXEC_H */
