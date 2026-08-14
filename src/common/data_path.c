#include "data_path.h"
#include "log_extra.h"
#include <unistd.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <errno.h>

#define LINGOS_ROOT "/LINGOS"

const char *lingos_data_root(void) {
    static int checked = 0;
    if (!checked) {
        if (access(LINGOS_ROOT, F_OK) != 0) {
            if (mkdir(LINGOS_ROOT, 0755) == 0) {
                LOG_INFO_T("DataPath", "Root", "Created", "created %s", LINGOS_ROOT);
            } else {
                LOG_WARN_T("DataPath", "Root", "Missing", "%s does not exist and cannot be created", LINGOS_ROOT);
            }
        }
        checked = 1;
    }
    return LINGOS_ROOT;
}