/**
 * @file    src/shell/dotted_cmd.h
 * @brief   点分命令（系统.动作）别名层 —— 先生决策 5【第 4 期收尾】
 * @version LN-0.9.0
 *
 * 先生裁决（docs/LINGOS-部署令-20261010.md 决策 5）：
 *   · 新增命令一律用「系统.动作」点分命名（唯一规范名）
 *   · 旧命令（空格分隔）**一律保留别名不删**（兼容铁律）
 *
 * 本层只做「点分 → 旧命令」的纯文本翻译，翻译后仍走既有分发链
 * （src/shell/shell.c:handle_builtin_command），新旧两种写法落到同一实现。
 *
 * 【2026-10-16】补 类目/系统/命令前缀 一致性：
 *   · dotted_cmd_list_systems()       —— 类目↔系统↔前缀↔点分命令（help systems）
 *   · dotted_cmd_consistency_report() —— 前缀必须归属已登记系统或明确保留的旧命令组
 */
#ifndef SHELL_DOTTED_CMD_H
#define SHELL_DOTTED_CMD_H

#include <stddef.h>

/* 点分命令翻译：命中返回 1 并写入 out（旧命令 + 原参数）；
 * 非点分命令 / 未收录的点分命令返回 0（原样交回既有分发链——不破坏旧行为） */
int dotted_cmd_translate(const char *input, char *out, size_t out_size);

/* 打印点分命令别名表（供 help dotted / 发现性） */
void dotted_cmd_list(void);

/* 表内条目数 */
int dotted_cmd_count(void);

/* 【2026-10-16】类目 ↔ 系统 ↔ 命令前缀 ↔ 点分命令（help systems）
 *   数据源：src/registry/registry_builtin.c（类目文档单一事实源）；
 *   无 CLI 命令面的系统如实标注（服务/守护/API 侧），不编造命令。 */
void dotted_cmd_list_systems(void);

/* 【2026-10-16】点分命令前缀一致性校验（真实可失败）：
 *   ① 每个点分别名的前缀必须 ∈ 已登记系统，或 ∈ 明确保留的旧命令组
 *   ② 统计各系统 CLI 覆盖（仅报告，不计违规）
 * 返回违规项数；报告写入 out，缓冲区不足返回 -1。 */
int dotted_cmd_consistency_report(char *out, size_t out_size);

#endif /* SHELL_DOTTED_CMD_H */
