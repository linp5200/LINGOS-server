/**
 * @file    src/common/error_codes.c
 * @brief   错误代码查找实现（硬编码版本，与 error_codes.h 匹配）
 * @version LN-B-4.3.0.0
 * @note    若项目有 gen_error_codes.py，可从此文件生成；若无，可直接使用此实现。
 */

#include "error_codes.h"
#include <string.h>

/* ============================================================
 * 错误代码表（与 error_codes.h 中的枚举保持一致）
 * ============================================================ */

static const error_code_t g_error_codes[] = {
    {"ERR_UNKNOWN_001", 0x00000000, "Unknown error", "未知错误"},
    {"ERR_MEM_001", 0xE0000001, "Memory allocation failed", "内存分配失败"},
    {"ERR_MEM_002", 0xE0000002, "Memory alignment fault", "内存对齐错误"},
    {"ERR_FS_001", 0xF0000001, "Root directory inaccessible", "根目录不可访问"},
    {"ERR_FS_002", 0xF0000002, "Configuration file corrupted", "配置文件损坏"},
    {"ERR_SYS_001", 0x10000001, "System initialization failed", "系统初始化失败"},
    {"ERR_SYS_002", 0x10000002, "Unhandled fatal signal", "未处理的致命信号"},
    {"ERR_AI_001", 0xA0000001, "Model file loading failed", "模型加载失败"},
    {"ERR_TUI_001", 0xB0000001, "Terminal initialization failed", "终端初始化失败"},
    {"ERR_SEC_001", 0x20000001, "Permission check failed", "权限检查失败"},
    {"ERR_PLUGIN_001", 0x30000001, "Plugin loading failed", "插件加载失败"},
    {"ERR_UPDATE_001", 0x40000001, "Update package corrupted", "更新包损坏"},
    {"ERR_VISION_001", 0x50000001, "Camera initialization failed", "摄像头初始化失败"},
    {"ERR_VOICE_001", 0x60000001, "Audio device initialization failed", "音频设备初始化失败"},
    {"ERR_NET_001", 0xC0000001, "Socket creation failed", "套接字创建失败"},
    {NULL, 0, NULL, NULL}
};

/* ============================================================
 * 查找函数实现
 * ============================================================ */

const error_code_t* error_code_find(const char *symbol) {
    if (!symbol) return NULL;
    for (int i = 0; g_error_codes[i].symbol; i++) {
        if (strcmp(g_error_codes[i].symbol, symbol) == 0) {
            return &g_error_codes[i];
        }
    }
    return NULL;
}

const error_code_t* error_code_find_by_id(uint32_t hex_id) {
    for (int i = 0; g_error_codes[i].symbol; i++) {
        if (g_error_codes[i].hex_id == hex_id) {
            return &g_error_codes[i];
        }
    }
    return NULL;
}

const char* error_code_get_desc(const char *symbol, const char *lang) {
    const error_code_t *ec = error_code_find(symbol);
    if (!ec) return "Unknown error";
    if (lang && strcmp(lang, "zh") == 0) {
        return ec->desc_zh ? ec->desc_zh : ec->desc_en;
    }
    return ec->desc_en;
}