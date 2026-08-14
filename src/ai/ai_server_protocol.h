#ifndef AI_SERVER_PROTOCOL_H
#define AI_SERVER_PROTOCOL_H

#include "lingos_config.h"

/* 命令常量 */
#define AI_CMD_NOOK_ASK         "nook_ask"
#define AI_CMD_SKILL_SCHEMAS    "skill_schemas"
#define AI_CMD_SKILL_EXEC       "skill_exec"
#define AI_CMD_USER_CONFIRM     "user_confirm"
#define AI_CMD_PING             "ping"

/* 状态常量 */
#define AI_STATUS_OK            "ok"
#define AI_STATUS_ERROR         "error"
#define AI_STATUS_CANCELLED     "cancelled"

/* 缓冲区大小 */
#define AI_MSG_MAX_LEN          (256 * 1024)
#define AI_RECV_BUF_SIZE        (1024 * 1024)

/* 分离的 Socket 路径 */
#define AI_SOCKET_PATH          LINGOS_RUN_DIR "/ai.sock"        /* lingos_linux <-> ai_server.py */
#define DAEMON_SOCKET_PATH      LINGOS_RUN_DIR "/daemon.sock"    /* ai_server.py <-> lingosd */

/* 其他路径 */
#define AI_SKILL_INDEX_PATH     "/LINGOS/skills/index.json"
#define AI_DEFAULT_TIMEOUT      120

#endif