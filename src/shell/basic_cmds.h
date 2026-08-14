#ifndef SHELL_BASIC_CMDS_H
#define SHELL_BASIC_CMDS_H

/* 内部实现的 ls 命令 */
void cmd_ls(const char *arg);

/* 内部实现的 cp 命令 */
void cmd_cp(const char *src, const char *dst);

/* 内部实现的 mv 命令 */
void cmd_mv(const char *src, const char *dst);

/* 内部实现的 pwd 命令 */
void cmd_pwd(void);

#endif