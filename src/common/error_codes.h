/**
 * @file    error_codes.h
 * @brief   错误代码枚举（由 tools/gen_error_codes.py 从 error_codes.json 生成）
 * @version LN-B-4.3.0.0
 * @note    此文件为自动生成，请勿手动修改
 */

#ifndef ERROR_CODES_H
#define ERROR_CODES_H

#include <stdint.h>

/* ============================================================
 * 错误代码枚举
 * ============================================================ */

#define ERR_CODE_MAP(XX) \
    XX(ERR_UNKNOWN_001, 0x00000000, "Unknown error") \
    XX(ERR_MEM_001, 0xE0000001, "Memory allocation failed") \
    XX(ERR_MEM_002, 0xE0000002, "Memory alignment fault") \
    XX(ERR_FS_001, 0xF0000001, "Root directory inaccessible") \
    XX(ERR_FS_002, 0xF0000002, "Configuration file corrupted") \
    XX(ERR_SYS_001, 0x10000001, "System initialization failed") \
    XX(ERR_SYS_002, 0x10000002, "Unhandled fatal signal") \
    XX(ERR_AI_001, 0xA0000001, "Model file loading failed") \
    XX(ERR_TUI_001, 0xB0000001, "Terminal initialization failed") \
    XX(ERR_SEC_001, 0x20000001, "Permission check failed") \
    XX(ERR_PLUGIN_001, 0x30000001, "Plugin loading failed") \
    XX(ERR_UPDATE_001, 0x40000001, "Update package corrupted") \
    XX(ERR_VISION_001, 0x50000001, "Camera initialization failed") \
    XX(ERR_VOICE_001, 0x60000001, "Audio device initialization failed") \
    XX(ERR_NET_001, 0xC0000001, "Socket creation failed")

/* ============================================================
 * 错误代码结构
 * ============================================================ */

typedef struct {
    const char *symbol;      /* 如 "ERR_MEM_001" */
    uint32_t hex_id;         /* 如 0xE0000001 */
    const char *desc_en;     /* 英文描述 */
    const char *desc_zh;     /* 中文描述 */
} error_code_t;

/* ============================================================
 * 查找函数
 * ============================================================ */

/**
 * @brief 根据错误符号查找错误信息
 * @param symbol 错误符号（如 "ERR_MEM_001"）
 * @return 指向错误信息的指针，未找到返回 NULL
 */
const error_code_t* error_code_find(const char *symbol);

/**
 * @brief 根据十六进制 ID 查找错误信息
 * @param hex_id 十六进制 ID（如 0xE0000001）
 * @return 指向错误信息的指针，未找到返回 NULL
 */
const error_code_t* error_code_find_by_id(uint32_t hex_id);

/**
 * @brief 获取错误代码描述（中英双语）
 * @param symbol 错误符号
 * @param lang 语言（"en" 或 "zh"）
 * @return 描述字符串，未找到返回 "Unknown error"
 */
const char* error_code_get_desc(const char *symbol, const char *lang);

#endif /* ERROR_CODES_H */