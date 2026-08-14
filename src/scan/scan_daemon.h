#ifndef SCAN_DAEMON_H
#define SCAN_DAEMON_H

int scan_daemon_start(void);
void scan_daemon_stop(void);
int scan_daemon_is_running(void);
int scan_daemon_trigger_now(void);

#endif