/**
 * @file    virus_scanner.h
 * @brief   病毒扫描器公共接口
 * @version LN-0.9.0-p5
 *
 * 【P5 接线上岗】virus_scanner.c 此前**没有头文件**：scan_result_t 定义在 .c 内，
 *   任何调用方都无法引用 → 完整实现却永远零调用者（先生铁律：完整 ≠ 上岗）。
 *   本头文件把枚举与 API 公开，供 security_wire.c / 应用安装链路调用。
 */

#ifndef SECURITY_VIRUS_SCANNER_H
#define SECURITY_VIRUS_SCANNER_H

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    SCAN_RESULT_CLEAN = 0,       /* 干净 */
    SCAN_RESULT_SUSPICIOUS = 1,  /* 可疑（解压失败/无法判定） */
    SCAN_RESULT_MALICIOUS = 2    /* 恶意（命中危险模式） */
} scan_result_t;

/**
 * @brief 扫描安装包（.lapt/.deb/.sub 等 tar.gz 包）
 * @param path 包路径
 * @return 扫描结果（包不存在视为 CLEAN，由调用方另行校验存在性）
 */
scan_result_t virus_scanner_scan_package(const char *path);

/**
 * @brief 结果名（"Clean" / "Suspicious" / "Malicious" / "Unknown"）
 */
const char* virus_scanner_result_name(scan_result_t result);

#ifdef __cplusplus
}
#endif

#endif /* SECURITY_VIRUS_SCANNER_H */
