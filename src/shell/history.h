#ifndef SHELL_HISTORY_H
#define SHELL_HISTORY_H

void history_init(void);
void history_add(const char *cmd);
const char *history_prev(void);
const char *history_next(void);
void history_save(void);
void history_load(void);

#endif