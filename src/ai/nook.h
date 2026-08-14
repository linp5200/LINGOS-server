#ifndef AI_NOOK_H
#define AI_NOOK_H

#include <stdint.h>
#include <time.h>
#include "nook_personality.h"

typedef enum { NOOK_MODE_NORMAL, NOOK_MODE_LOCKDOWN, NOOK_MODE_DANGER } nook_mode_t;
typedef enum { TASK_PRIO_LOW, TASK_PRIO_MED, TASK_PRIO_HIGH, TASK_PRIO_URGENT } task_priority_t;
typedef enum { TASK_STATUS_PENDING, TASK_STATUS_RUNNING, TASK_STATUS_COMPLETED, TASK_STATUS_FAILED, TASK_STATUS_ABORTED } task_status_t;

typedef struct {
    uint32_t task_id;
    char desc[256];          /* 【修复】固定数组，避免外部指针悬垂 */
    task_priority_t pri;
    task_status_t status;
    char ai[64];             /* 【修复】固定数组，避免外部指针悬垂 */
} nook_task_t;

typedef struct {
    char ai_name[64];        /* 【修复】固定数组，避免外部指针悬垂 */
    uint32_t perms;
    uint32_t task_id;
    uint8_t cross;
    time_t granted_at;       /* 【安全冗余】授权时间戳 */
    int duration_sec;        /* 【安全冗余】有效时长（秒），0 = 永久 */
} nook_authorization_t;

int nook_ask_ollama_with_details(const char *prompt, const char *model, char *resp,
                                 uint32_t len, int timeout_sec, int show_thinking,
                                 int show_tool_calls, int show_tool_results);
                                 
void nook_init(void);
void nook_set_mode(nook_mode_t m);
nook_mode_t nook_get_mode(void);
const char* nook_mode_str(void);
int  nook_dispatch_task(const char *d, task_priority_t p, const char *a);
void nook_report_task_result(uint32_t tid, task_status_t s);
int  nook_authorize_ai(const char *n, uint32_t perms, uint32_t tid, uint8_t cross);
int  nook_revoke_ai(const char *n);
int  nook_check_authorization(const char *n, uint32_t p);
void nook_set_personality(nook_personality_t *p);
nook_personality_t* nook_get_personality(void);
/* 以下函数现在通过 socket 与 Python 端通信，实现已修改但保留接口以兼容 shell.c */
void nook_set_user_name(const char *n);
const char* nook_get_user_name(void);
int  nook_repair(const char *e, const char *patch);
int  nook_ask_ollama(const char *prompt, const char *model, char *resp, uint32_t len, int timeout_sec);
/* 【批次F】流式 AI 对话（过程事件 + 逐块实时显示） */
int  nook_ask_stream(const char *prompt, const char *model, char *resp, uint32_t len, int timeout_sec);
void nook_show_status(void);

#endif