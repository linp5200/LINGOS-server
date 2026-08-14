#ifndef SHELL_BASIC_CMDS_H
#define SHELL_BASIC_CMDS_H

void cmd_ls(const char *arg);
void cmd_cp(const char *src, const char *dst);
void cmd_mv(const char *src, const char *dst);
void cmd_pwd(void);

#endif