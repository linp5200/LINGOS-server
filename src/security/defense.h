/**
 * @file    src/security/defense.h
 * @brief   主动防御系统头文件
 * @version LN-B-4.3.0.0
 * @changes 添加 <stdint.h> 以修正 uint8_t/uint32_t 未定义错误
 */

#ifndef SECURITY_DEFENSE_H
#define SECURITY_DEFENSE_H

#include <stdint.h>          /* 新增：修复 uint8_t/uint32_t 未定义 */

/* ============================================================
 * 防御配置结构
 * ============================================================ */
typedef struct {
    uint8_t shadow_enabled;
    uint8_t dark_enabled;
    uint8_t absolute_protect;
    uint8_t behavior_monitoring;
    int anomaly_threshold;
    char anomaly_algorithm[64];
} defense_config_t;

/* ============================================================
 * 公共 API
 * ============================================================ */
void defense_init(void);
void defense_shadow_mode(int enable);
void defense_dark_mode(int enable);
void defense_absolute_protect(void);
void defense_set_anomaly_threshold(int threshold);
int defense_get_anomaly_threshold(void);
void defense_show_status(void);

/* 配置读写（供配置向导使用） */
int defense_save_config(void);
int defense_load_config(defense_config_t *cfg);

/* 异常检测 */
int defense_check_anomaly(const char *ai_name, const char *skill_name,
                          char *result, uint32_t result_len);

#endif /* SECURITY_DEFENSE_H */