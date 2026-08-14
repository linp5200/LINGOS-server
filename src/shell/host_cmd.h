#ifndef SHELL_HOST_CMD_H
#define SHELL_HOST_CMD_H

int exec_host_command(const char *cmd);
void host_handle_sigquit(void);

#endif