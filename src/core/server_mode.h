/**
 * @file    src/core/server_mode.h
 * @brief   server mode（先生设定 2026-09-19 · P2 实施）
 * @version LN-0.7.0
 *
 * 设定摘要（开发设定记录 §1）：
 *   · 定义：只显示日志、不接受输入（控制键除外）
 *   · 控制键唯一：Ctrl-Q / Q —— 关闭 server mode 会停掉服务器
 *   · 持久化：server mode on 后保持（重启不丢——server_mode.json）
 *   · 服务器模式包：默认启用；常规关闭方式无效；停止只需 `server mode stop`
 *   · 客户端 off 首确认：首次由服务器端确认、后续永久设备级免确认
 *   · 危机时严禁退出（一切行为为人身安全让路）
 */

#ifndef CORE_SERVER_MODE_H
#define CORE_SERVER_MODE_H

#ifdef __cplusplus
extern "C" {
#endif

/** 初始化（启动时调用——读 server_mode.json + startup 模式联动） */
int  server_mode_init(void);

/** 是否处于 server mode（enabled 状态） */
int  server_mode_is_active(void);

/** 是否服务器模式包（package_mode——常规关闭无效） */
int  server_mode_is_package(void);

/** 开启/关闭 server mode（持久化到 server_mode.json） */
int  server_mode_set_enabled(int enabled);

/** 危机是否进行中（读 crisis_state.json 的 active） */
int  server_mode_crisis_active(void);

/** 尝试停止服务器（危机时拒绝）。成功=已触发优雅退出流程（不返回） */
int  server_mode_request_stop(const char *source);

/** server mode 主循环（阻塞）：日志尾随显示 + Ctrl-Q/Q 输入门控 */
void server_mode_run(void);

/** shell 命令处理：`server mode [on|off|stop|status]`（args 为 "mode ..." 之后的部分） */
int  server_mode_command(const char *args);

#ifdef __cplusplus
}
#endif

#endif /* CORE_SERVER_MODE_H */
