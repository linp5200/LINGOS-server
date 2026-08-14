#ifndef SECURITY_UID_H
#define SECURITY_UID_H
#include <stddef.h>
typedef enum { UID_TYPE_USER, UID_TYPE_AI_MAIN, UID_TYPE_AI_SUB } uid_type_t;
static inline void uid_generate(uid_type_t t, const char *n, char *out, size_t s) { if(out&&s) out[0]=0; }
static inline int uid_verify(const char *u) { return 0; }
static inline int uid_is_blacklisted(const char *u) { return 0; }
#endif
