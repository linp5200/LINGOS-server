/**
 * @file    test_update.c
 * @brief   系统更新模块测试（适配 test_framework）
 * @version LN-B-5.0.0.0
 * @changes 安全字符串替换；双文支持
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <errno.h>
#include "../update/manifest.h"
#include "../update/apply_changes.h"
#include "../common/safe_string.h"
#include "../common/lang.h"
#include "test_framework.h"

static char test_root[512];

static void create_test_env(void) {
    safe_snprintf(test_root, sizeof(test_root), "/tmp/lingos_test_%d", getpid());
    mkdir(test_root, 0755);
    chdir(test_root);
    mkdir("system", 0755);
    mkdir("system/config", 0755);
}

static void cleanup_test_env(void) {
    char cmd[1024];
    safe_snprintf(cmd, sizeof(cmd), "rm -rf %s", test_root);
    system(cmd);
}

static int test_manifest_parse(void) {
    const char *manifest_content =
        "{"
        "\"version\": \"2.0.0.1\","
        "\"previous_version\": \"2.0.0.0\","
        "\"components\": ["
        "  { \"type\": \"binary\", \"name\": \"lingos\", \"version\": \"2.0.0.1\", \"changes\": ["
        "    { \"source\": \"bin/lingos_linux\", \"dest\": \"/bin/lingos_linux\", \"backup\": 1 }"
        "  ]}"
        "],"
        "\"requires_reboot\": false"
        "}";
    char tmp_path[256];
    safe_snprintf(tmp_path, sizeof(tmp_path), "%s/manifest.json", test_root);
    FILE *fp = fopen(tmp_path, "w");
    if (!fp) {
        printf("[%s] manifest_parse: %s\n", tr("FAIL", "失败"), tr("cannot create manifest file", "无法创建清单文件"));
        return -1;
    }
    fwrite(manifest_content, 1, strlen(manifest_content), fp);
    fclose(fp);

    manifest_t m = {0};
    if (manifest_parse(tmp_path, &m) != 0) {
        printf("[%s] manifest_parse: %s\n", tr("FAIL", "失败"), tr("parse failed", "解析失败"));
        return -1;
    }
    if (m.version == NULL || strcmp(m.version, "2.0.0.1") != 0) {
        printf("[%s] manifest_parse: %s\n", tr("FAIL", "失败"), tr("version mismatch", "版本不匹配"));
        manifest_free(&m);
        return -1;
    }
    if (m.component_count != 1) {
        printf("[%s] manifest_parse: %s\n", tr("FAIL", "失败"), tr("component count mismatch", "组件数量不匹配"));
        manifest_free(&m);
        return -1;
    }
    manifest_free(&m);
    printf("[%s] manifest_parse\n", tr("PASS", "通过"));
    return 0;
}

static int test_apply_add(void) {
    const char *manifest_content =
        "{"
        "\"version\": \"2.0.0.1\","
        "\"components\": ["
        "  { \"type\": \"config\", \"name\": \"test\", \"version\": \"1.0\", \"changes\": ["
        "    { \"source\": \"newfile.txt\", \"dest\": \"/system/config/test.txt\", \"backup\": 0 }"
        "  ]}"
        "],"
        "\"requires_reboot\": false"
        "}";
    char tmp_path[256], update_dir[256];
    safe_snprintf(tmp_path, sizeof(tmp_path), "%s/manifest.json", test_root);
    safe_snprintf(update_dir, sizeof(update_dir), "%s/update", test_root);
    mkdir(update_dir, 0755);

    FILE *fp = fopen(tmp_path, "w");
    if (!fp) {
        printf("[%s] apply_add: %s\n", tr("FAIL", "失败"), tr("cannot create manifest", "无法创建清单"));
        return -1;
    }
    fwrite(manifest_content, 1, strlen(manifest_content), fp);
    fclose(fp);

    char src_path[256];
    safe_snprintf(src_path, sizeof(src_path), "%s/newfile.txt", update_dir);
    fp = fopen(src_path, "w");
    if (!fp) {
        printf("[%s] apply_add: %s\n", tr("FAIL", "失败"), tr("cannot create source file", "无法创建源文件"));
        return -1;
    }
    fputs("test content", fp);
    fclose(fp);

    manifest_t m = {0};
    if (manifest_parse(tmp_path, &m) != 0) {
        printf("[%s] apply_add: %s\n", tr("FAIL", "失败"), "manifest_parse failed");
        return -1;
    }
    int ret = apply_changes(update_dir, &m);
    manifest_free(&m);
    if (ret != 0) {
        printf("[%s] apply_add: apply_changes returned %d\n", tr("FAIL", "失败"), ret);
        return -1;
    }

    char dst_path[256];
    safe_snprintf(dst_path, sizeof(dst_path), "%s/system/config/test.txt", test_root);
    if (access(dst_path, F_OK) != 0) {
        printf("[%s] apply_add: %s\n", tr("FAIL", "失败"), tr("target file not created", "目标文件未创建"));
        return -1;
    }
    printf("[%s] apply_add\n", tr("PASS", "通过"));
    return 0;
}

static int test_apply_delete(void) {
    char dst_path[256];
    safe_snprintf(dst_path, sizeof(dst_path), "%s/system/config/delete_me.txt", test_root);
    FILE *fp = fopen(dst_path, "w");
    if (fp) {
        fputs("to be deleted", fp);
        fclose(fp);
    }

    const char *manifest_content =
        "{"
        "\"version\": \"2.0.0.1\","
        "\"components\": ["
        "  { \"type\": \"config\", \"name\": \"test\", \"version\": \"1.0\", \"changes\": ["
        "    { \"source\": null, \"dest\": \"/system/config/delete_me.txt\", \"backup\": 0 }"
        "  ]}"
        "],"
        "\"requires_reboot\": false"
        "}";
    char tmp_path[256], update_dir[256];
    safe_snprintf(tmp_path, sizeof(tmp_path), "%s/manifest.json", test_root);
    safe_snprintf(update_dir, sizeof(update_dir), "%s/update", test_root);
    mkdir(update_dir, 0755);

    fp = fopen(tmp_path, "w");
    if (!fp) {
        printf("[%s] apply_delete: %s\n", tr("FAIL", "失败"), tr("cannot create manifest", "无法创建清单"));
        return -1;
    }
    fwrite(manifest_content, 1, strlen(manifest_content), fp);
    fclose(fp);

    manifest_t m = {0};
    if (manifest_parse(tmp_path, &m) != 0) {
        printf("[%s] apply_delete: %s\n", tr("FAIL", "失败"), "manifest_parse failed");
        return -1;
    }
    int ret = apply_changes(update_dir, &m);
    manifest_free(&m);
    if (ret != 0) {
        printf("[%s] apply_delete: apply_changes returned %d\n", tr("FAIL", "失败"), ret);
        return -1;
    }

    if (access(dst_path, F_OK) == 0) {
        printf("[%s] apply_delete: %s\n", tr("FAIL", "失败"), tr("target file not deleted", "目标文件未删除"));
        return -1;
    }
    printf("[%s] apply_delete\n", tr("PASS", "通过"));
    return 0;
}

static int test_rollback(void) {
    printf("[%s] %s\n", tr("PASS", "通过"), tr("rollback (stub)", "回滚（占位）"));
    return 0;
}

void register_update_tests(void) {
    test_case_t tests[] = {
        {tr("manifest_parse", "清单解析"), tr("Test manifest parsing", "测试清单解析"), test_manifest_parse},
        {tr("apply_add", "应用添加"), tr("Test apply_changes add", "测试 apply_changes 添加"), test_apply_add},
        {tr("apply_delete", "应用删除"), tr("Test apply_changes delete", "测试 apply_changes 删除"), test_apply_delete},
        {tr("rollback", "回滚"), tr("Test rollback stub", "测试回滚占位"), test_rollback}
    };
    for (size_t i = 0; i < sizeof(tests)/sizeof(tests[0]); i++) {
        test_register(&tests[i]);
    }
}

#ifdef RUN_UPDATE_TESTS_MAIN
int main(int argc, char **argv) {
    (void)argc; (void)argv;
    create_test_env();
    register_update_tests();
    cleanup_test_env();
    return 0;
}
#endif