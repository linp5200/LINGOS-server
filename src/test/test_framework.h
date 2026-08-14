#ifndef TEST_FRAMEWORK_H
#define TEST_FRAMEWORK_H

#include <stdint.h>

typedef struct {
    const char *name;
    const char *description;
    int (*func)(void);
} test_case_t;

void test_register(const test_case_t *tc);
int test_get_count(void);
const test_case_t *test_get_case(int index);
int test_run_range(const char *range_str, int *total, int *passed);
void test_list(void);
void test_init(void);
void register_all_test_cases(void);

#endif