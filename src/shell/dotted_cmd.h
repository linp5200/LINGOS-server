/**
 * @file    src/shell/dotted_cmd.h
 * @brief   点分命令（系统.动作）别名层 —— 先生决策 5【第 4 期收尾】
 * @version LN-0.8.0
 *
 * 先生裁决（docs/LINGOS-部署令-20261010.md 决策 5）：
 *   · 新增命令一律用「系统.动作」点分命名（唯一规范名）
 *   · 旧命令（空格分隔）**一律保留别名不删**（兼容铁律）
 *
 * 本层只做「点分 → 旧命令」的纯文本翻译，翻译后仍走既有分发链
 * （src/shell/shell.c:handle_builtin_command），因此新旧两种写法
 * 落到同一实现，不存在第二套逻辑（无假数据、无占位）。
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

#endif /* SHELL_DOTTED_CMD_H */
