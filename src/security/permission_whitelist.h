/**
 * @file    permission_whitelist.h
 * @brief   权限白名单管理头文件
 * @version LN-B-4.3.0.0
 */

#ifndef SECURITY_PERMISSION_WHITELIST_H
#define SECURITY_PERMISSION_WHITELIST_H

#include <stdint.h>
#include <stddef.h>

typedef enum {
    PERM_UNKNOWN = 0,
    PERM_FILE_READ,
    PERM_FILE_WRITE,
    PERM_FILE_DELETE,
    PERM_FILE_EXEC,
    PERM_FILE_LIST,
    PERM_NET_CONNECT,
    PERM_NET_BIND,
    PERM_NET_DNS,
    PERM_MEM_ALLOC,
    PERM_MEM_MAP,
    PERM_CPU_QUOTA,
    PERM_CPU_PRIORITY,
    PERM_CAMERA,
    PERM_MICROPHONE,
    PERM_BLUETOOTH,
    PERM_USB,
    PERM_SERIAL,
    PERM_SYSCALL,
    PERM_ALL
} permission_type_t;

int permission_whitelist_load(void);
int permission_whitelist_save(void);
int permission_whitelist_check(const char *app_id, permission_type_t perm);
int permission_whitelist_grant(const char *app_id, permission_type_t perm);
int permission_whitelist_revoke(const char *app_id, permission_type_t perm);
int permission_whitelist_list(const char *app_id, char *out, size_t out_len);
const char* permission_type_to_string(permission_type_t perm);
permission_type_t permission_type_from_string(const char *str);

#endif /* SECURITY_PERMISSION_WHITELIST_H */