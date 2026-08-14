/**
 * @file    test_framework.c
 * @brief   测试框架核心
 * @version LN-B-5.0.0.0
 * @changes 安全字符串替换；双文支持
 */

#include "test_framework.h"
#include "../lib/log_extra.h"
#include "../common/data_path.h"
#include "../common/safe_string.h"
#include "../common/lang.h"
#include "../drivers/uart.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>

#define MAX_TEST_CASES 256
static const test_case_t *test_cases[MAX_TEST_CASES];
static int test_count = 0;

void test_register(const test_case_t *tc) {
    if (test_count >= MAX_TEST_CASES) {
        LOG_ERROR_T("Test", "Register", "Overflow", "too many test cases");
        return;
    }
    test_cases[test_count++] = tc;
    LOG_DEBUG_T("Test", "Register", "OK", "registered '%s'", tc->name);
}

int test_get_count(void) {
    return test_count;
}

const test_case_t *test_get_case(int index) {
    if (index < 0 || index >= test_count) return NULL;
    return test_cases[index];
}

static void log_test_result(const char *test_name, int result, const char *output_file) {
    const char *root = lingos_data_root();
    char log_dir[512];
    safe_snprintf(log_dir, sizeof(log_dir), "%s/Debug", root);
    if (access(log_dir, F_OK) != 0) mkdir(log_dir, 0755);

    char log_path[512];
    safe_snprintf(log_path, sizeof(log_path), "%s/test.log", log_dir);
    FILE *fp = fopen(log_path, "a");
    if (!fp) return;

    time_t now = time(NULL);
    char time_str[64];
    strftime(time_str, sizeof(time_str), "%Y-%m-%d %H:%M:%S", localtime(&now));
    fprintf(fp, "[%s] %s: %s\n", time_str, test_name, result ? "FAILED" : "PASSED");
    if (output_file) fprintf(fp, "  Output captured in %s\n", output_file);
    fclose(fp);
}

int test_run_range(const char *range_str, int *total, int *passed) {
    int failures = 0;
    int tot = 0;
    int pass = 0;
    int range[256][2];
    int range_count = 0;

    if (strcmp(range_str, "all") == 0) {
        range[0][0] = 1;
        range[0][1] = test_count;
        range_count = 1;
    } else {
        char *str = strdup(range_str);
        char *token = strtok(str, ",");
        while (token && range_count < 256) {
            if (strchr(token, '-')) {
                int start, end;
                if (sscanf(token, "%d-%d", &start, &end) == 2) {
                    range[range_count][0] = start;
                    range[range_count][1] = end;
                    range_count++;
                }
            } else {
                int num = atoi(token);
                range[range_count][0] = num;
                range[range_count][1] = num;
                range_count++;
            }
            token = strtok(NULL, ",");
        }
        free(str);
    }

    for (int i = 0; i < range_count; i++) {
        for (int idx = range[i][0]; idx <= range[i][1]; idx++) {
            if (idx < 1 || idx > test_count) {
                uart_puts(tr("Invalid test number: ", "无效测试编号："));
                char buf[8];
                safe_snprintf(buf, sizeof(buf), "%d", idx);
                uart_puts(buf);
                uart_puts("\n");
                continue;
            }
            const test_case_t *tc = test_cases[idx-1];
            tot++;
            uart_puts(tr("Running test: ", "运行测试："));
            uart_puts(tc->name);
            uart_puts(" ... ");
            int ret = tc->func();
            if (ret == 0) {
                uart_puts(tr("PASS\n", "通过\n"));
                pass++;
            } else {
                uart_puts(tr("FAIL\n", "失败\n"));
                failures++;
            }
            log_test_result(tc->name, ret, NULL);
            if (idx != range[i][1] || i != range_count-1) {
                uart_puts(tr("Press Enter to continue, ^Q to quit: ", "按回车继续，^Q 退出："));
                char c = uart_getc();
                if (c == 17) {
                    uart_puts("\n");
                    break;
                }
                uart_puts("\n");
            }
        }
    }

    if (total) *total = tot;
    if (passed) *passed = pass;
    return failures;
}

void test_list(void) {
    uart_puts(tr("Available test cases:\n", "可用测试用例：\n"));
    for (int i = 0; i < test_count; i++) {
        char buf[256];
        safe_snprintf(buf, sizeof(buf), "  %3d: %s - %s\n", i+1, test_cases[i]->name, test_cases[i]->description);
        uart_puts(buf);
    }
}

void test_init(void) {
    LOG_INFO_T("Test", "Init", "OK", "test framework ready");
}