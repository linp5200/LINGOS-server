#ifndef FS_FILE_INTEGRITY_H
#define FS_FILE_INTEGRITY_H

int integrity_check_file(const char *relpath, const char *marker);
int integrity_check_all(void);
int integrity_check_required(void);

#endif