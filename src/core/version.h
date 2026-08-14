#ifndef CORE_VERSION_H
#define CORE_VERSION_H
const char *version_get(void);
int version_set(const char *new_version);
int version_ensure(void);
#endif
