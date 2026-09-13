#ifndef CORE_BACKUP_H
#define CORE_BACKUP_H

#include <stddef.h>

/* 备份整个 /LINGOS 目录，返回备份路径 */
/* is_manual: 1=手动备份(manual_前缀), 0=AI自动备份(pre_repair_前缀) */
int backup_system(char *backup_path, size_t path_len, int is_manual);

/* 从指定备份恢复 */
int restore_backup(const char *backup_path);

/* 清理过期备份（按策略） */
int cleanup_backups(void);

/* 【0.6.0 S15】备份敏感文件加密 / 还原（envelope + backup.key） */
int backup_encrypt_sensitive(const char *backup_path);
int backup_decrypt_sensitive(const char *backup_root);

/* 云端同步（占位） */
int sync_to_cloud(const char *backup_path);

#endif