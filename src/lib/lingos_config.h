#ifndef LINGOS_CONFIG_H
#define LINGOS_CONFIG_H

/* 选择平台：Linux 用户态 */
#define LINGOS_PLATFORM_LINUX

/* 运行时目录定义 */
#define LINGOS_RUN_DIR "/LINGOS/run"
#define LINGOS_AI_SOCKET_PATH LINGOS_RUN_DIR "/lingos_ai.sock"
#define LINGOS_AI_PID_PATH LINGOS_RUN_DIR "/ai_server.pid"

/* 【2026-09-19】单实例锁（flock——防双实例端口冲突）
 *   主程序持 lingos.lock；supervisor 持 supervisor.lock。
 *   EXIT_ALREADY_RUNNING：主程序检测到已有实例时的退出码——
 *   supervisor 收到该码将停止重启循环（防互抢端口）。 */
#define LINGOS_INSTANCE_LOCK   LINGOS_RUN_DIR "/lingos.lock"
#define LINGOS_SUPERVISOR_LOCK LINGOS_RUN_DIR "/supervisor.lock"
#define EXIT_ALREADY_RUNNING   75

#endif