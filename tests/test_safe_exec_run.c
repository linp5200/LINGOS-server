/* safe_exec 执行层测试 —— 验证「不经 shell」时元字符失效 */
#include "safe_exec.h"
#include <stdio.h>
#include <string.h>

static int pass = 0, fail = 0;

static void run_and_show(const char *cmd, const char *desc, int want_contain_cmd) {
    char out[8192] = {0};
    int nc = 0;
    char reason[256] = {0};
    int rc = safe_exec_run_cmdline(cmd, out, sizeof(out), 10, &nc, reason, sizeof(reason));

    /* 若命令里的 "echo" 后接了 shell 注入（如 ; rm 或 | cat），
     * 由于不经 shell，应该被当作**普通参数原样输出**，而不是被执行 */
    int len = (int)strlen(out);
    printf("  %-40s rc=%-3d out=%.90s\n", desc, rc, len ? out : "(空)");
    (void)want_contain_cmd;
}

int main(void) {
    printf("=== safe_exec 执行层测试（不经 shell）===\n\n");

    printf("[1] 正常命令（应成功执行）\n");
    {
        char out[512] = {0}; int nc = 0; char r[128] = {0};
        int rc = safe_exec_run_cmdline("echo hello-lingos", out, sizeof(out), 5, &nc, r, sizeof(r));
        int ok = (rc == 0 && strstr(out, "hello-lingos") != NULL);
        ok ? pass++ : fail++;
        printf("  %s echo hello-lingos → rc=%d out=%s", ok ? "✓" : "✗", rc, out);
    }
    {
        char out[1024] = {0}; int nc = 0; char r[128] = {0};
        int rc = safe_exec_run_cmdline("uname -s", out, sizeof(out), 5, &nc, r, sizeof(r));
        int ok = (rc == 0 && strlen(out) > 0);
        ok ? pass++ : fail++;
        printf("  %s uname -s → rc=%d out=%s", ok ? "✓" : "✗", rc, out);
    }

    printf("\n[2] ★ 关键：shell 元字符应【失效】（不被解释）\n");
    {
        /* `echo a ; echo b` —— 旧 popen 会输出两行；execvp 应把 ';' 当普通参数 */
        char out[1024] = {0}; int nc = 0; char r[128] = {0};
        int rc = safe_exec_run_cmdline("echo a ; echo b", out, sizeof(out), 5, &nc, r, sizeof(r));
        int has_semicolon = (strstr(out, ";") != NULL);
        int not_executed = (strstr(out, "a ; echo b") != NULL);  /* 整串作为参数输出 */
        int ok = (rc == 0 && has_semicolon && not_executed);
        ok ? pass++ : fail++;
        printf("  %s 'echo a ; echo b' → rc=%d\n", ok ? "✓" : "✗", rc);
        printf("      输出: %s\n", out);
        printf("      → 含 ';' 且未被拆成两条 = 元字符失效 ✅\n");
    }
    {
        /* `echo $(whoami)` —— 旧 popen 会执行 whoami；execvp 应原样输出 */
        char out[1024] = {0}; int nc = 0; char r[128] = {0};
        int rc = safe_exec_run_cmdline("echo $(id)", out, sizeof(out), 5, &nc, r, sizeof(r));
        int literal = (strstr(out, "$(id)") != NULL);
        int ok = (rc == 0 && literal);
        ok ? pass++ : fail++;
        printf("  %s 'echo $(id)' → rc=%d 原样输出=%s\n", ok ? "✓" : "✗", rc, literal ? "是" : "否");
        printf("      输出: %s\n", out);
    }
    {
        /* 管道不应生效 */
        char out[1024] = {0}; int nc = 0; char r[128] = {0};
        int rc = safe_exec_run_cmdline("echo x | wc -l", out, sizeof(out), 5, &nc, r, sizeof(r));
        int literal = (strstr(out, "|") != NULL);
        int ok = (rc == 0 && literal);
        ok ? pass++ : fail++;
        printf("  %s 'echo x | wc -l' → rc=%d 管道字面=%s\n", ok ? "✓" : "✗", rc, literal ? "是" : "否");
    }
    {
        /* 重定向不应生效 */
        char out[1024] = {0}; int nc = 0; char r[128] = {0};
        int rc = safe_exec_run_cmdline("echo hi > /tmp/should_not_exist_lingos", out, sizeof(out), 5, &nc, r, sizeof(r));
        FILE *f = fopen("/tmp/should_not_exist_lingos", "r");
        int notcreated = (f == NULL);
        if (f) fclose(f);
        int ok = notcreated;
        ok ? pass++ : fail++;
        printf("  %s 重定向 '>' 未生效（文件未创建）=%s\n", ok ? "✓" : "✗", notcreated ? "是" : "否");
    }

    printf("\n[3] 危险命令应被策略拦截（不执行）\n");
    {
        char out[512] = {0}; int nc = 0; char r[128] = {0};
        int rc = safe_exec_run_cmdline("rm -rf / --no-preserve-root", out, sizeof(out), 5, &nc, r, sizeof(r));
        int ok = (rc == -3);
        ok ? pass++ : fail++;
        printf("  %s 'rm -rf / --no-preserve-root' → rc=%d (%s)\n",
               ok ? "✓" : "✗", rc, rc == -3 ? "已拦截" : "未拦截!");
    }
    {
        char out[512] = {0}; int nc = 0; char r[128] = {0};
        int rc = safe_exec_run_cmdline("cat /etc/shadow", out, sizeof(out), 5, &nc, r, sizeof(r));
        int ok = (rc == -3);
        ok ? pass++ : fail++;
        printf("  %s 'cat /etc/shadow' → rc=%d (%s)\n", ok ? "✓" : "✗", rc, rc == -3 ? "已拦截" : "未拦截!");
    }

    printf("\n=== 结果: 通过 %d / 失败 %d ===\n", pass, fail);
    return fail == 0 ? 0 : 1;
}
