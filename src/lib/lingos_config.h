#ifndef LINGOS_CONFIG_H
#define LINGOS_CONFIG_H

/* 选择平台：Linux 用户态 */
#define LINGOS_PLATFORM_LINUX

/* 运行时目录定义 */
#define LINGOS_RUN_DIR "/LINGOS/run"
#define LINGOS_AI_SOCKET_PATH LINGOS_RUN_DIR "/lingos_ai.sock"
#define LINGOS_AI_PID_PATH LINGOS_RUN_DIR "/ai_server.pid"

#endif