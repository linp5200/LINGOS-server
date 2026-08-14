#ifndef AI_AI_MASTER_H
#define AI_AI_MASTER_H

#include <stdint.h>
#include "../common/types.h"

typedef enum {
    MASTER_CODE,
    MASTER_GUARD,
    MASTER_GENERAL
} master_ai_type_t;

typedef struct {
    uint8_t approved;
    uint8_t needs_revision;
    char    suggestion[256];
} master_review_t;

void ai_master_init(void);
int ai_master_review(master_ai_type_t master_type,
                     void *sub_result,  // 原 sub_ai_result_t* 已废弃
                     master_review_t *review);
const char *ai_master_type_name(master_ai_type_t type);

#endif