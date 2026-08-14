#ifndef UPDATE_BACKUP_RESTORE_H
#define UPDATE_BACKUP_RESTORE_H

const char* backup_lingos(void);
int restore_lingos(void);
void backup_restore_set_test_root(const char *test_root);

#endif