#ifndef CORE_APP_RUNNER_H
#define CORE_APP_RUNNER_H

int app_start(const char *app_name);
int app_stop(const char *app_name);
int app_is_running(const char *app_name);
char *app_get_logs(const char *app_name);
const char *get_app_dir(const char *app_name);
char *read_entry_point(const char *app_name);

#endif