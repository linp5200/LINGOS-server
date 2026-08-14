#ifndef DAEMON_APP_DAEMON_H
#define DAEMON_APP_DAEMON_H

int app_daemon_start(void);
void app_daemon_stop(void);
int app_daemon_is_running(void);
void app_daemon_monitor_add(const char *app_name);

#endif