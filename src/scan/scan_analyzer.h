#ifndef SCAN_ANALYZER_H
#define SCAN_ANALYZER_H

#include <stdint.h>

typedef struct {
    int total_skills;
    int high_risk_count;
    int medium_risk_count;
    int low_risk_count;
    char summary[512];
} scan_result_t;

int scan_perform_full(scan_result_t *result);
const scan_result_t *scan_get_last_result(void);

#endif