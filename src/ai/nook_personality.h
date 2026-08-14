#ifndef AI_NOOK_PERSONALITY_H
#define AI_NOOK_PERSONALITY_H

#include <stdint.h>   /* 提供 uint8_t */

typedef enum { GENDER_MALE, GENDER_FEMALE } personality_gender_t;
typedef enum { VOICE_MALE, VOICE_FEMALE } personality_voice_t;

typedef struct {
    const char          *name;
    personality_gender_t gender;
    personality_voice_t  voice_tone;
    uint8_t              style_strict;
    uint8_t              style_warmth;
    uint8_t              style_speed;
} nook_personality_t;

#endif