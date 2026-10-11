/**
 * @file    tools/lingosctl.c
 * @brief   LINGOS 统一调用 API 命令行客户端（先生 P5：OpenAPI + SDK + CLI）
 * @version LN-0.9.0
 * @par     协议：本工具向 lingosd 的 Unix socket 发送
 *            {"id":N,"command":"config.get","args":{...},"rely":true,"return":"json"}
 *          并打印 {"id":N,"code":0,"message":"success","data":{...}}
 * @par     用法示例：
 *           lingosctl system.version
 *           lingosctl config.set key_path=language value=zh
 *           lingosctl config.set key_path=language value=zh --rely=false   # 只改内存不落盘
 *           lingosctl debug.set_log_level level=debug
 *           lingosctl --list                     # = api.describe
 *           lingosctl --openapi > openapi.json   # = api.openapi
 *           lingosctl --raw system.events limit=5
 *           lingosctl --json '{"key_path":"user_name","value":"先生"}' config.set
 * @par     退出码：0=成功；1=API 返回非 0；2=连接/协议错误
 * @note    socket 默认 /LINGOS/run/daemon.sock（与 DAEMON_SOCKET_PATH 同值），
 *          可用 --socket PATH 或环境变量 LINGOS_DAEMON_SOCKET 覆盖。
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/socket.h>
#include <sys/un.h>

#include "cJSON.h"

#define DEFAULT_SOCKET   "/LINGOS/run/daemon.sock"
#define RESP_MAX         (1024 * 1024)

static void usage(const char *prog) {
    printf("LINGOS 统一调用 API 客户端（%s）\n", "LN-0.9.0");
    printf("用法: %s [选项] <command> [key=value ...]\n\n", prog);
    printf("选项:\n");
    printf("  --socket PATH     指定 daemon socket（默认 %s）\n", DEFAULT_SOCKET);
    printf("  --json JSON       直接给出 args 对象（与 key=value 互斥，后者优先）\n");
    printf("  --rely=false      只执行当前命令，跳过依赖业务调用\n");
    printf("  --id N            指定请求 id（默认 1）\n");
    printf("  --raw             打印原始响应 JSON\n");
    printf("  --list            等价 api.describe\n");
    printf("  --openapi         等价 api.openapi（输出 OpenAPI 3.0 描述）\n");
    printf("  -h, --help        显示本帮助\n\n");
    printf("示例:\n");
    printf("  %s system.version\n", prog);
    printf("  %s config.set key_path=language value=zh\n", prog);
    printf("  %s config.set key_path=language value=zh --rely=false\n", prog);
}

/* 值解析：true/false/null/数字/JSON → 原生类型；其余按字符串 */
static cJSON* parse_value(const char *v) {
    char *end = NULL;
    double d;
    if (!v) return cJSON_CreateNull();
    if (strcmp(v, "true") == 0) return cJSON_CreateBool(1);
    if (strcmp(v, "false") == 0) return cJSON_CreateBool(0);
    if (strcmp(v, "null") == 0) return cJSON_CreateNull();
    if (v[0] == '{' || v[0] == '[') {
        cJSON *j = cJSON_Parse(v);
        if (j) return j;
    }
    d = strtod(v, &end);
    if (end && end != v && *end == '\0') return cJSON_CreateNumber(d);
    return cJSON_CreateString(v);
}

static int connect_daemon(const char *path) {
    int fd;
    struct sockaddr_un addr;
    if (!path || strlen(path) >= sizeof(addr.sun_path)) return -1;
    fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        fprintf(stderr, "lingosctl: socket() 失败: %s\n", strerror(errno));
        return -1;
    }
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, path, sizeof(addr.sun_path) - 1);
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        fprintf(stderr, "lingosctl: 连接 %s 失败: %s\n", path, strerror(errno));
        fprintf(stderr, "         （lingosd 未运行？或换 --socket PATH）\n");
        close(fd);
        return -1;
    }
    return fd;
}

static int exchange(int fd, const char *req, char **resp_out) {
    char *buf;
    size_t used = 0;
    ssize_t n;
    if (write(fd, req, strlen(req)) < 0 || write(fd, "\n", 1) < 0) {
        fprintf(stderr, "lingosctl: 写入失败: %s\n", strerror(errno));
        return -1;
    }
    buf = malloc(RESP_MAX);
    if (!buf) return -1;
    while (used < RESP_MAX - 1) {
        n = read(fd, buf + used, RESP_MAX - 1 - used);
        if (n < 0) {
            if (errno == EINTR) continue;
            fprintf(stderr, "lingosctl: 读取失败: %s\n", strerror(errno));
            free(buf);
            return -1;
        }
        if (n == 0) break;
        used += (size_t)n;
        if (memchr(buf, '\n', used)) break;
    }
    buf[used] = '\0';
    *resp_out = buf;
    return 0;
}

int main(int argc, char **argv) {
    const char *sock_path = getenv("LINGOS_DAEMON_SOCKET");
    const char *command = NULL;
    const char *json_args = NULL;
    int rely = 1;
    int raw = 0;
    int req_id = 1;
    int i;
    cJSON *req = NULL;
    cJSON *args = NULL;
    char *req_str = NULL;
    char *resp_str = NULL;
    cJSON *resp = NULL;
    int fd;
    int rc = 0;

    if (!sock_path) sock_path = DEFAULT_SOCKET;

    for (i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (strcmp(a, "-h") == 0 || strcmp(a, "--help") == 0) { usage(argv[0]); return 0; }
        else if (strcmp(a, "--list") == 0) { command = "api.describe"; }
        else if (strcmp(a, "--openapi") == 0) { command = "api.openapi"; }
        else if (strcmp(a, "--raw") == 0) { raw = 1; }
        else if (strcmp(a, "--socket") == 0 && i + 1 < argc) { sock_path = argv[++i]; }
        else if (strncmp(a, "--socket=", 9) == 0) { sock_path = a + 9; }
        else if (strcmp(a, "--json") == 0 && i + 1 < argc) { json_args = argv[++i]; }
        else if (strcmp(a, "--rely=false") == 0) { rely = 0; }
        else if (strcmp(a, "--rely=true") == 0) { rely = 1; }
        else if (strcmp(a, "--id") == 0 && i + 1 < argc) { req_id = atoi(argv[++i]); }
        else if (a[0] == '-' && a[1] == '-') { fprintf(stderr, "lingosctl: 未知选项 %s\n", a); usage(argv[0]); return 2; }
        else if (!command) { command = a; }
        else {
            /* key=value → args */
            const char *eq = strchr(a, '=');
            if (!eq) { fprintf(stderr, "lingosctl: 参数应为 key=value: %s\n", a); return 2; }
            if (!args) args = cJSON_CreateObject();
            {
                char key[128];
                size_t klen = (size_t)(eq - a);
                if (klen >= sizeof(key)) klen = sizeof(key) - 1;
                memcpy(key, a, klen);
                key[klen] = '\0';
                cJSON_AddItemToObject(args, key, parse_value(eq + 1));
            }
        }
    }

    if (!command) { usage(argv[0]); return 2; }
    if (!args && json_args) {
        args = cJSON_Parse(json_args);
        if (!args) { fprintf(stderr, "lingosctl: --json 解析失败\n"); return 2; }
    }

    req = cJSON_CreateObject();
    if (!req) return 2;
    cJSON_AddNumberToObject(req, "id", req_id);
    cJSON_AddStringToObject(req, "command", command);
    if (args) cJSON_AddItemToObject(req, "args", args);
    cJSON_AddBoolToObject(req, "rely", rely ? 1 : 0);
    cJSON_AddStringToObject(req, "return", "json");
    req_str = cJSON_PrintUnformatted(req);
    cJSON_Delete(req);
    if (!req_str) return 2;

    fd = connect_daemon(sock_path);
    if (fd < 0) { free(req_str); return 2; }

    if (exchange(fd, req_str, &resp_str) != 0) { close(fd); free(req_str); return 2; }
    close(fd);
    free(req_str);

    if (raw) {
        printf("%s\n", resp_str ? resp_str : "(空响应)");
        free(resp_str);
        return 0;
    }

    resp = cJSON_Parse(resp_str ? resp_str : "");
    free(resp_str);
    if (!resp) {
        fprintf(stderr, "lingosctl: 响应不是合法 JSON\n");
        return 2;
    }
    {
        cJSON *code = cJSON_GetObjectItem(resp, "code");
        cJSON *msg = cJSON_GetObjectItem(resp, "message");
        cJSON *data = cJSON_GetObjectItem(resp, "data");
        int code_val = cJSON_IsNumber(code) ? code->valueint : -1;
        char *data_str = data ? cJSON_Print(data) : NULL;
        printf("code: %d (%s)\n", code_val,
               cJSON_IsString(msg) ? msg->valuestring : (code_val == 0 ? "success" : "error"));
        if (data_str) {
            printf("data: %s\n", data_str);
            free(data_str);
        }
        rc = (code_val == 0) ? 0 : 1;
    }
    cJSON_Delete(resp);
    return rc;
}
