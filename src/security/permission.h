#ifndef SECURITY_PERMISSION_H
#define SECURITY_PERMISSION_H

#include <stdint.h>
#include <stddef.h>          // 新增：提供 size_t
#include "../common/types.h"

/* 旧权限枚举（保留兼容） */
typedef enum {
    PERM_LOCATION, PERM_CAMERA, PERM_RECORD_AUDIO,
    PERM_RECORD_SCREEN, PERM_ACCELEROMETER, PERM_PHONE_STATE,
    PERM_INSTALLED_APPS, PERM_EXTERNAL_STORAGE, PERM_NETWORK_CONTROL,
    PERM_BLUETOOTH_CONTROL, PERM_SCAN_BLUETOOTH, PERM_LAUNCH_APP,
    PERM_INSTALL_APP, PERM_JUMP_APP, PERM_BACKGROUND_DATA,
    PERM_BACKGROUND_TASK, PERM_AUTO_START, PERM_COUNT
} permission_t;

typedef enum {
    AUTH_MODE_DENY, AUTH_MODE_ALLOW_ONCE,
    AUTH_MODE_ALLOW_WHILE, AUTH_MODE_ALLOW_ALWAYS, AUTH_MODE_SHADOW
} auth_mode_t;

typedef enum {
    BG_MODE_DENY, BG_MODE_ALLOW, BG_MODE_ALLOW_MINUTES, BG_MODE_ALLOW_ONCE
} bg_mode_t;

/* 旧接口（保留兼容） */
void permission_init(void);
int permission_check(uint32_t app_id, permission_t perm);
auth_mode_t permission_request(uint32_t app_id, permission_t perm);
void permission_set_mode(uint32_t app_id, permission_t perm, auth_mode_t mode);
void permission_set_bg_mode(uint32_t app_id, bg_mode_t mode, uint32_t minutes);
void permission_reload_config(void);
uint32_t permission_get_bg_remaining(uint32_t app_id);
const char* permission_get_name(permission_t perm);
const char* auth_mode_get_name(auth_mode_t mode);

/* 新增令牌接口 */
int permission_check_token(const char *token, const char *required_perm);
int permission_grant_token(const char *role, const char **perms, int perm_count,
                           int expire_seconds, char *out_token, size_t out_len);
int permission_revoke_token(const char *token);
void permission_set_token_task_id(const char *token, const char *task_id);

/* 旧权限位掩码（仅参考） */
typedef uint64_t perm_flags_t;
#define PERM_NONE             0x0000000000000000ULL
#define PERM_READ_FILE        0x0000000000000001ULL
#define PERM_WRITE_FILE       0x0000000000000002ULL
#define PERM_DELETE_FILE      0x0000000000000004ULL
#define PERM_EXEC_COMMAND     0x0000000000000008ULL
#define PERM_LIST_PROCESS     0x0000000000000010ULL
#define PERM_KILL_PROCESS     0x0000000000000020ULL
#define PERM_INSTALL_PACKAGE  0x0000000000000040ULL
#define PERM_REMOVE_PACKAGE   0x0000000000000080ULL
#define PERM_MANAGE_SERVICE   0x0000000000000100ULL
#define PERM_MODIFY_CONFIG    0x0000000000000200ULL
#define PERM_MANAGE_USER      0x0000000000000400ULL
#define PERM_REBOOT_SYSTEM    0x0000000000000800ULL
#define PERM_READ_MEMORY      0x0000000000001000ULL
#define PERM_WRITE_MEMORY     0x0000000000002000ULL
#define PERM_DELEGATE_SUBAI   0x0000000000010000ULL
#define PERM_CRYPTO_ACCESS    0x0000000000020000ULL
#define PERM_ADMIN            0xFFFFFFFFFFFFFFFFULL

#endif