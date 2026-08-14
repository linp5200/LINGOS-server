#ifndef AI_NOOK_IDLE_H
#define AI_NOOK_IDLE_H
#include <stdint.h>          /* 新增 */
#include "../common/types.h"

typedef enum {
    IDLE_CHECK_LOG,
    IDLE_CHECK_NETWORK,
    IDLE_CHECK_SYSTEM,
    IDLE_CHECK_PROCESS,
    IDLE_CHECK_PERMISSION,
    IDLE_CHECK_DEFENSE,
    IDLE_CHECK_SKILL,
    IDLE_CHECK_CONFIG,
    IDLE_CHECK_COUNT
} idle_check_type_t;

typedef struct {
    uint8_t  enabled;
    uint32_t idle_timeout_ticks;
    uint64_t last_command_tick;
    uint64_t last_idle_check_tick;
    uint32_t idle_interval_ticks;
    uint8_t  checks_enabled[IDLE_CHECK_COUNT];
    uint8_t  check_running;
} idle_state_t;

void nook_idle_init(void);
void nook_idle_feed(uint64_t current_tick);
void nook_idle_poll(uint64_t current_tick);
void nook_idle_set_enabled(uint8_t enabled);
void nook_idle_show_status(void);

int idle_check_log(char *result, uint32_t len);
int idle_check_network(char *result, uint32_t len);
int idle_check_system(char *result, uint32_t len);
int idle_check_process(char *result, uint32_t len);
int idle_check_permission(char *result, uint32_t len);
int idle_check_defense(char *result, uint32_t len);
int idle_check_skill(char *result, uint32_t len);
int idle_check_config(char *result, uint32_t len);
#endif