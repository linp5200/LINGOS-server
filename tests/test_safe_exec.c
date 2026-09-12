/* 安全执行模块测试（safe_exec）—— 验证 OWASP 三层防护
 * 编译: gcc -Isrc -Isrc/common -Isrc/security -Isrc/lib test_safe_exec.c
 *       src/security/safe_exec.c src/lib/log_extra.c ... -o t
 */
#include "safe_exec.h"
#include <stdio.h>
#include <string.h>

static int pass = 0, fail = 0;

static void expect(const char *cmd, safe_exec_verdict_t want) {
    char reason[256] = {0};
    safe_exec_verdict_t got = safe_exec_check(cmd, reason, sizeof(reason));
    const char *names[] = {"ALLOW", "NEED_CONFIRM", "DENY"};
    int ok = (got == want);
    if (ok) pass++; else fail++;
    printf("  %s  %-34s → %-12s %s\n",
           ok ? "✓" : "✗", cmd,
           names[got], ok ? "" : reason);
}

int main(void) {
    printf("=== safe_exec 策略检查测试 ===\n\n");

    printf("[1] 危险命令 → 应拒绝(DENY)\n");
    expect("rm -rf /", SAFE_EXEC_DENY);
    expect("rm -rf /*", SAFE_EXEC_DENY);
    expect("dd if=/dev/zero of=/dev/sda", SAFE_EXEC_DENY);
    expect("mkfs.ext4 /dev/sda1", SAFE_EXEC_DENY);
    expect(":(){ :|:& };:", SAFE_EXEC_DENY);
    expect("cat /etc/shadow", SAFE_EXEC_DENY);
    expect("echo x > /dev/sda", SAFE_EXEC_DENY);
    expect("shutdown -h now", SAFE_EXEC_DENY);

    printf("\n[2] 空白绕过变体 → 仍应拒绝（我方案的关键）\n");
    expect("rm  -rf   /", SAFE_EXEC_DENY);
    expect("RM -RF /", SAFE_EXEC_DENY);
    expect("rm\t-rf\t/", SAFE_EXEC_DENY);

    printf("\n[3] 高风险 → 应需确认(NEED_CONFIRM)\n");
    expect("rm /tmp/x", SAFE_EXEC_NEED_CONFIRM);
    expect("kill 1234", SAFE_EXEC_NEED_CONFIRM);

    printf("\n[4] 白名单内只读命令 → 应允许(ALLOW)\n");
    expect("ls -la /LINGOS", SAFE_EXEC_ALLOW);
    expect("cat /LINGOS/version.txt", SAFE_EXEC_ALLOW);
    expect("ps aux", SAFE_EXEC_ALLOW);
    expect("df -h", SAFE_EXEC_ALLOW);
    expect("uname -a", SAFE_EXEC_ALLOW);

    printf("\n[5] 白名单外 → BALANCED 模式需确认\n");
    expect("nc -l 1234", SAFE_EXEC_NEED_CONFIRM);

    printf("\n[6] 严格模式（白名单制，先生可切换）\n");
    safe_exec_set_mode(SAFE_EXEC_MODE_STRICT);
    expect("nc -l 1234", SAFE_EXEC_DENY);
    expect("ls -la", SAFE_EXEC_ALLOW);
    safe_exec_set_mode(SAFE_EXEC_MODE_BALANCED);

    printf("\n=== 结果: 通过 %d / 失败 %d ===\n", pass, fail);
    return fail == 0 ? 0 : 1;
}
