/**
 * @file    absolute_protect.h
 * @brief   绝对保护模式接口
 * @version LN-0.9.0-p5
 *
 * 【P5 接线上岗】absolute_protect.c 此前无头文件（仅 absolute_protect.c 内部
 *   通过 extern 声明 defense_mode_set_force）→ 触发函数全仓零调用者，
 *   defense.c 的绝对保护横幅只是「喊口号」：没有任何输入真的被阻断。
 *   本头文件公开 API，由 defense.c: defense_absolute_protect() 真实调用：
 *   横幅 → absolute_protect_trigger() → defense_mode_set(ABSOLUTE)
 *   → execution_gate_check() 拒绝所有外部输入（真实阻断）。
 */

#ifndef SECURITY_ABSOLUTE_PROTECT_H
#define SECURITY_ABSOLUTE_PROTECT_H

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 触发绝对保护（幂等；置 ABSOLUTE 模式 + 最高日志级别 + 横幅）
 * @param source 触发来源（"shell" / "alert" / 模块名）
 * @return 0 成功（含已激活），-1 失败
 */
int absolute_protect_trigger(const char *source);

/** @brief 绝对保护是否处于激活态 */
int absolute_protect_is_active(void);

/** @brief 是否已排定自动关闭 */
int absolute_protect_should_auto_close(void);

/** @brief 自动关闭绝对保护（降级到暗影模式） */
int absolute_protect_auto_close(void);

/** @brief 标记自动关闭待处理 */
void absolute_protect_set_auto_close_pending(void);

/**
 * @brief 该来源的输入是否应被阻断（绝对保护态下除系统必需来源外全部阻断）
 * @return 1 阻断，0 放行
 */
int absolute_protect_should_block_input(const char *source);

#ifdef __cplusplus
}
#endif

#endif /* SECURITY_ABSOLUTE_PROTECT_H */
