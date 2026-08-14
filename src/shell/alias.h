#ifndef SHELL_ALIAS_H
#define SHELL_ALIAS_H

void alias_set(const char *name, const char *cmd);
void alias_unset(const char *name);
const char *alias_get(const char *name);
void alias_list(void);
void alias_save(void);
void alias_load(void);

#endif