#ifndef HEALTH_SYSTEM_HEALTH_H
#define HEALTH_SYSTEM_HEALTH_H

void system_health_command(void);
void health_check_and_alert(void);
int get_memory_usage(void);
int get_disk_usage(const char *path);
void get_load_avg(double *load1, double *load5, double *load15);
int check_python(void);
int check_ai_backend(void);
int check_network(void);

#endif