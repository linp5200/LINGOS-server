#ifndef HEALTH_HEALTH_TREND_H
#define HEALTH_HEALTH_TREND_H

#include <stdint.h>
#include <time.h>

#define HEALTH_HISTORY_MAX_DAYS 30

typedef struct {
    time_t timestamp;
    int    mem_usage;
    int    disk_usage;
    double load_avg;
    int    python_ok;
    int    ai_backend_ok;
    int    net_ok;
} health_record_t;

int health_trend_init(void);
int health_trend_record(const health_record_t *record);
int health_trend_get_history(health_record_t *out, int max_days);
int health_trend_analyze(char *output, size_t output_len);
int health_trend_cleanup(void);

#endif