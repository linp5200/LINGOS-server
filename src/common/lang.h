/**
 * @file    src/common/lang.h
 * @brief   多语言支持头文件
 * @version LN-0.4.3
 * @changes 新增 lang_set_system_default() 声明
 */

#ifndef COMMON_LANG_H
#define COMMON_LANG_H

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    LANG_EN = 0,
    LANG_ZH = 1
} lang_t;

extern lang_t g_language;
extern int _current_lang;   /* 供 ai_master.c 使用（0=EN, 1=ZH） */
extern int lingos_force_english;

/**
 * @brief 翻译函数
 * @param en 英文字符串
 * @param zh 中文字符串
 * @return 根据当前语言返回对应字符串
 */
const char *tr(const char *en, const char *zh);

/**
 * @brief 初始化语言系统
 * @return 0 成功
 */
int lang_init(void);

/**
 * @brief 获取当前语言
 * @return 当前语言枚举值
 */
lang_t lang_get_current(void);

/**
 * @brief 设置语言（并保存到配置文件）
 * @param lang 语言枚举值
 * @return 0 成功，-1 失败
 */
int lang_set(lang_t lang);

/**
 * @brief 重新加载语言（配置重载时调用）
 */
void lang_reload(void);

/**
 * @brief 根据系统 LANG 环境变量设置临时语言
 */
void lang_set_system_default(void);

/**
 * @brief 从配置文件加载语言
 * @param out_lang 输出语言
 * @return 0 成功，-1 失败
 */
int lang_load_from_config(lang_t *out_lang);

/**
 * @brief 交互式语言选择
 * @param force 强制显示选择菜单
 * @return 选择的语言
 */
lang_t lang_interactive_select(int force);

/**
 * @brief 检查是否为中文
 * @return 1 是中文，0 不是
 */
int is_chinese(void);

/**
 * @brief 获取语言名称
 * @return "中文" 或 "English"
 */
const char* get_language_name(void);

/**
 * @brief 设置语言（简化接口）
 * @param lang 语言字符串 ("en" 或 "zh")
 */
void set_language(const char *lang);

/**
 * @brief 获取当前语言字符串
 * @return "en" 或 "zh"
 */
const char* get_language(void);

#ifdef __cplusplus
}
#endif

#endif /* COMMON_LANG_H */