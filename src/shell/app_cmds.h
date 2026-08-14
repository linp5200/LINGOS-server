#ifndef SHELL_APP_CMDS_H
#define SHELL_APP_CMDS_H

void app_install_command(const char *package_path);
void app_uninstall_command(const char *app_name);
void app_list_command(void);
void app_run_command(const char *app_name);
void app_stop_command(const char *app_name);
void app_logs_command(const char *app_name);
void app_dispatch(const char *cmd_line);  /* 供 shell 调用的入口 */

#endif