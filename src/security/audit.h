#ifndef SECURITY_AUDIT_H
#define SECURITY_AUDIT_H

#include <stdint.h>
#include "../common/types.h"

#define AUDIT_HASH_SIZE 64

typedef struct {
    uint64_t timestamp;
    char     uid[32];
    char     source[32];
    char     skill_name[64];
    char     args[256];
    char     result[256];
    int      ret_code;
    char     risk_level[16];
    uint8_t  confirmed;
    char     prev_hash[AUDIT_HASH_SIZE];
} audit_entry_t;

void audit_init(void);
void audit_log(const char *uid, const char *source, const char *skill_name,
               const char *args, const char *result, int ret_code,
               const char *risk_level, uint8_t confirmed);
void audit_dump(char *buf, uint32_t buf_len);
int  audit_count(void);
int  audit_verify(char *error_msg, uint32_t msg_len);
int audit_save_to_file(const char *path);
int audit_load_from_file(const char *path);
/* 新增修复函数，strategy: 1=truncate, 2=recalc */
int audit_repair(int strategy, char *error_msg, uint32_t msg_len);
/* 新增导出 */
char* audit_export_json(void);

#endif