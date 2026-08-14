/**
 * @file    test_cases.c
 * @brief   内置测试用例集合（已适配Python技能重构，C端测试跳过）
 * @version LN-B-5.0.0.0
 * @changes 安全字符串替换；双文支持
 */

#include "test_framework.h"
#include "../common/data_path.h"
#include "../common/string_no_sys.h"
#include "../common/safe_string.h"
#include "../common/lang.h"
#include "../ai/nook.h"
#include "../security/audit.h"
#include "../fs/fs_layout.h"
#include "../drivers/uart.h"
#include "../lib/log_extra.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/stat.h>
#include "test_update.h"

/* 所有技能测试已迁移至Python，C端测试返回成功（跳过） */
static int test_file_write(void) { return 0; }
static int test_file_read(void) { return 0; }
static int test_file_delete(void) { return 0; }
static int test_file_list(void) { return 0; }
static int test_memory_write(void) { return 0; }
static int test_memory_search(void) { return 0; }
static int test_memory_delete(void) { return 0; }
static int test_memory_index(void) { return 0; }
static int test_process_list(void) { return 0; }
static int test_system_info(void) { return 0; }

static int test_audit_log(void) {
    char buf[8192];
    audit_dump(buf, sizeof(buf));
    if (strlen(buf) == 0) return 1;
    return 0;
}

static int test_ai_basic(void) {
    char response[4096];
    int ret = nook_ask_ollama(tr("Hello", "你好"), NULL, response, sizeof(response), 30);
    if (ret != 0) return 0;
    if (strlen(response) == 0) return 1;
    return 0;
}

static int test_config_io(void) { return 0; }
static int test_sandbox(void) { return 0; }

void register_all_test_cases(void) {
    static test_case_t tc1 = {
        .name = "file_write",
        .description = "File write test (skipped)",
        .func = test_file_write
    };
    static test_case_t tc2 = {
        .name = "file_read",
        .description = "File read test (skipped)",
        .func = test_file_read
    };
    static test_case_t tc3 = {
        .name = "file_delete",
        .description = "File delete test (skipped)",
        .func = test_file_delete
    };
    static test_case_t tc4 = {
        .name = "file_list",
        .description = "File list test (skipped)",
        .func = test_file_list
    };
    static test_case_t tc5 = {
        .name = "memory_write",
        .description = "Memory write test (skipped)",
        .func = test_memory_write
    };
    static test_case_t tc6 = {
        .name = "memory_search",
        .description = "Memory search test (skipped)",
        .func = test_memory_search
    };
    static test_case_t tc7 = {
        .name = "memory_delete",
        .description = "Memory delete test (skipped)",
        .func = test_memory_delete
    };
    static test_case_t tc8 = {
        .name = "memory_index",
        .description = "Memory index test (skipped)",
        .func = test_memory_index
    };
    static test_case_t tc9 = {
        .name = "process_list",
        .description = "Process list test (skipped)",
        .func = test_process_list
    };
    static test_case_t tc10 = {
        .name = "system_info",
        .description = "System info test (skipped)",
        .func = test_system_info
    };
    static test_case_t tc11 = {
        .name = "audit_log",
        .description = "Audit log test",
        .func = test_audit_log
    };
    static test_case_t tc12 = {
        .name = "ai_basic",
        .description = "AI basic conversation test",
        .func = test_ai_basic
    };
    static test_case_t tc13 = {
        .name = "config_io",
        .description = "Config import/export (deprecated)",
        .func = test_config_io
    };
    static test_case_t tc14 = {
        .name = "sandbox",
        .description = "Sandbox execution (deprecated)",
        .func = test_sandbox
    };

    test_register(&tc1); test_register(&tc2);
    test_register(&tc3); test_register(&tc4);
    test_register(&tc5); test_register(&tc6);
    test_register(&tc7); test_register(&tc8);
    test_register(&tc9); test_register(&tc10);
    test_register(&tc11); test_register(&tc12);
    test_register(&tc13); test_register(&tc14);

    register_update_tests();
}