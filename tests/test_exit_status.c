/* exit_status 状态机功能测试（2026-09-19 修复验证）
 *
 * 验证目标（修复模式不再死机制）：
 *   1. 首次启动 → 不异常
 *   2. 运行中"崩溃"（脏标记残留）→ 下次启动检出异常 + crash_count
 *   3. 干净退出 → 下次启动不异常 + crash_count 归零
 *   4. 信号停止（Stopped (signal 15)）→ 不异常
 *   5. 连续崩溃累计（3 次 → crash_count=3 → 触发自动修复阈值）
 *   6. 旧版文件兼容（clean=1 + reason=Running → 视为异常）
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "exit_status.h"

static int failures = 0;
static void expect(int cond, const char *msg) {
    printf("  [%s] %s\n", cond ? "PASS" : "FAIL", msg);
    if (!cond) failures++;
}

int main(void) {
    setenv("LINGOS_ROOT", "/tmp/lingostest", 1);
    system("rm -rf /tmp/lingostest");

    printf("== boot1: 首次启动（无文件）==\n");
    exit_status_init(NULL);
    expect(exit_status_check_abnormal(NULL) == 0, "first start not abnormal");
    exit_status_mark_running();   /* 运行脏标记 */
    /* 模拟：运行中崩溃（无任何 mark 调用） */

    printf("== boot2: 崩溃后启动 → 应检出 ==\n");
    exit_status_init(NULL);
    expect(exit_status_check_abnormal(NULL) == 1, "crash detected (abnormal)");
    expect(exit_status_get()->crash_count == 1, "crash_count=1");
    exit_status_clear_abnormal(); /* 模拟修复完成 */
    exit_status_mark_running();

    printf("== boot3: 干净退出后启动 → 不应异常 ==\n");
    exit_status_mark_clean(0, "Normal exit");
    exit_status_init(NULL);
    expect(exit_status_check_abnormal(NULL) == 0, "clean exit not abnormal");
    expect(exit_status_get()->crash_count == 0, "crash_count reset");
    exit_status_mark_running();

    printf("== boot4: 信号停止后启动 → 不应异常 ==\n");
    exit_status_mark_clean(143, "Stopped (signal 15)");
    exit_status_init(NULL);
    expect(exit_status_check_abnormal(NULL) == 0, "signal stop not abnormal");
    exit_status_mark_running();

    printf("== 连续崩溃累计 ==\n");
    exit_status_init(NULL);
    expect(exit_status_get()->crash_count == 1, "crash chain #1 → count=1");
    exit_status_mark_running();
    exit_status_init(NULL);
    expect(exit_status_get()->crash_count == 2, "crash chain #2 → count=2");
    exit_status_mark_running();
    exit_status_init(NULL);
    expect(exit_status_get()->crash_count == 3, "crash chain #3 → count=3 (auto-repair threshold)");
    /* 修复文案（崩溃原因显示） */
    {
        char buf[512];
        exit_status_format_message(exit_status_get(), "zh", buf, sizeof(buf));
        printf("  [info] 修复菜单文案:\n----\n%s\n----\n", buf);
    }

    printf("== 恢复：干净退出清空 ==\n");
    exit_status_mark_running();
    exit_status_mark_clean(0, "Normal exit");
    exit_status_init(NULL);
    expect(exit_status_check_abnormal(NULL) == 0, "recovered after clean exit");
    expect(exit_status_get()->crash_count == 0, "count reset after recovery");

    printf("== 旧版文件兼容（clean=1 + reason=Running）==\n");
    {
        FILE *f = fopen("/tmp/lingostest/state/exit_status.json", "w");
        if (f) {
            fprintf(f, "# LING OS Exit Status (auto-generated)\n"
                       "last_start_time=100\nlast_exit_time=0\nlast_exit_code=0\n"
                       "is_clean_exit=1\nlast_exit_reason=Running\n"
                       "crash_count=0\nfirst_crash_time=0\n");
            fclose(f);
        }
        exit_status_init(NULL);
        expect(exit_status_check_abnormal(NULL) == 1, "legacy 'Running' file detected as abnormal (upgrade safe)");
    }

    printf("\n%s (%d failures)\n", failures ? "FAILED" : "ALL PASS", failures);
    return failures ? 1 : 0;
}
