/**
 * @file    config_backup.h
 * @brief   配置备份与回滚
 * @version LN-B-3.8.0.0
 */
#ifndef SHELL_CONFIG_CONFIG_BACKUP_H
#define SHELL_CONFIG_CONFIG_BACKUP_H

int backup_config(void);
int restore_config(const char *backup_path);
char** list_config_backups(void);

#endif