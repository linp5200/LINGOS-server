/**
 * @file    scan_analyzer.c
 * @brief   技能风险扫描分析器（已适配Python重构，分析功能简化）
 * @version 3.0.0.0
 */

#include "scan_analyzer.h"
#include "../common/data_path.h"
#include "log_extra.h"
#include "uart.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <unistd.h>

static scan_result_t last_result = {0};

static int analyze_skill(const char *skill_name, char *risk_level, size_t len) {
    /* 技能分析已迁移至Python，C端直接返回低风险 */
    (void)skill_name;
    if (len > 0) snprintf(risk_level, len, "low");
    return 0;
}

int scan_perform_full(scan_result_t *result) {
    if (!result) return -1;
    memset(result, 0, sizeof(scan_result_t));
    const char *root = lingos_data_root();
    char skills_dir[512];
    snprintf(skills_dir, sizeof(skills_dir), "%s/skills/custom", root);
    DIR *d = opendir(skills_dir);
    if (!d) {
        LOG_WARN_T("ScanAnalyzer", "Full", "NoCustom", "no custom skills directory");
        snprintf(result->summary, sizeof(result->summary), "No custom skills found");
        return 0;
    }
    struct dirent *entry;
    while ((entry = readdir(d))) {
        if (entry->d_name[0] == '.') continue;
        if (strcmp(entry->d_name, "index.json") == 0) continue;
        char skill_path[512];
        snprintf(skill_path, sizeof(skill_path), "%s/%s", skills_dir, entry->d_name);
        if (access(skill_path, F_OK) != 0) continue;
        char risk[32] = {0};
        if (analyze_skill(entry->d_name, risk, sizeof(risk)) == 0) {
            result->total_skills++;
            if (strcmp(risk, "critical") == 0 || strcmp(risk, "high") == 0)
                result->high_risk_count++;
            else if (strcmp(risk, "medium") == 0)
                result->medium_risk_count++;
            else
                result->low_risk_count++;
        }
        usleep(10000);
    }
    closedir(d);
    snprintf(result->summary, sizeof(result->summary),
             "Total: %d, High: %d, Medium: %d, Low: %d",
             result->total_skills, result->high_risk_count,
             result->medium_risk_count, result->low_risk_count);
    LOG_INFO_T("ScanAnalyzer", "Full", "Done", "%s", result->summary);
    if (result->total_skills > 0) {
        last_result = *result;
    }
    return 0;
}

const scan_result_t *scan_get_last_result(void) {
    return &last_result;
}