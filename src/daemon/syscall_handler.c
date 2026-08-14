/**
 * @file    syscall_handler.c
 * @brief   原子系统调用实现（无授权、无交互）
 * @version LN-B-4.2.0.0
 * @changes 修复 JSON 控制字符转义；统一使用 cJSON 序列化；详细日志
 */

#include "syscall_handler.h"
#include "log_extra.h"
#include "cJSON.h"
#include "data_path.h"
#include "safe_string.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <net/if.h>
#include <sys/ioctl.h>
#include <ifaddrs.h>
#include <sys/sysinfo.h>
#include <sys/wait.h>
#include <time.h>

/* ============================================================
 * 内部辅助：控制字符转义（E4 修复）
 * ============================================================ */
static char* escape_json_string(const char *input) {
    LOG_DEBUG_T("Syscall", "EscapeJSON", "Enter", "input_len=%zu", input ? strlen(input) : 0);
    if (!input) {
        LOG_DEBUG_T("Syscall", "EscapeJSON", "NullInput", "input is NULL");
        return strdup("");
    }

    size_t len = strlen(input);
    size_t out_len = len + 1;
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)input[i];
        if (c < 0x20 || c == 0x7f) {
            /* 控制字符：转义为 \uXXXX 或 \n \t \r 等 */
            if (c == '\n') out_len += 1;      /* \n 是2字符 */
            else if (c == '\t') out_len += 1; /* \t 是2字符 */
            else if (c == '\r') out_len += 1; /* \r 是2字符 */
            else if (c == '\b') out_len += 1; /* \b 是2字符 */
            else if (c == '\f') out_len += 1; /* \f 是2字符 */
            else out_len += 6;                /* \uXXXX 是6字符 */
        } else if (c == '"' || c == '\\') {
            out_len += 1;  /* 添加转义反斜杠 */
        }
    }

    char *out = malloc(out_len);
    if (!out) {
        LOG_ERROR_T("Syscall", "EscapeJSON", "MallocFail", "malloc(%zu) failed", out_len);
        return strdup("");
    }

    char *p = out;
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)input[i];
        if (c < 0x20 || c == 0x7f) {
            switch (c) {
                case '\n': *p++ = '\\'; *p++ = 'n'; break;
                case '\t': *p++ = '\\'; *p++ = 't'; break;
                case '\r': *p++ = '\\'; *p++ = 'r'; break;
                case '\b': *p++ = '\\'; *p++ = 'b'; break;
                case '\f': *p++ = '\\'; *p++ = 'f'; break;
                default:
                    *p++ = '\\';
                    *p++ = 'u';
                    *p++ = '0';
                    *p++ = '0';
                    *p++ = "0123456789ABCDEF"[(c >> 4) & 0xF];
                    *p++ = "0123456789ABCDEF"[c & 0xF];
                    break;
            }
        } else if (c == '"') {
            *p++ = '\\';
            *p++ = '"';
        } else if (c == '\\') {
            *p++ = '\\';
            *p++ = '\\';
        } else {
            *p++ = (char)c;
        }
    }
    *p = '\0';

    size_t final_len = p - out;
    LOG_DEBUG_T("Syscall", "EscapeJSON", "OK", "input_len=%zu, output_len=%zu", len, final_len);
    return out;
}

/* ============================================================
 * 【修复】popen 输出动态读取：循环读完 + 上限保护 + UTF-8 边界截断 + 截断标记
 * ============================================================ */
static char* popen_read_all(FILE *fp, size_t max_bytes, const char *trunc_mark) {
    size_t cap = 8192;
    size_t used = 0;
    char *buf = malloc(cap);
    if (!buf) {
        LOG_ERROR_T("Syscall", "PopenRead", "MallocFail", "malloc(%zu) failed", cap);
        return NULL;
    }
    while (used < max_bytes) {
        size_t n = fread(buf + used, 1, cap - used, fp);
        if (n == 0) break;  /* EOF 或错误 */
        used += n;
        if (used == cap && used < max_bytes) {
            cap *= 2;
            if (cap > max_bytes) cap = max_bytes;
            char *nb = realloc(buf, cap);
            if (!nb) break;
            buf = nb;
        }
    }
    buf[used] = '\0';
    /* UTF-8 边界回退：避免截断在多字节字符中间（无效 JSON） */
    while (used > 0 && ((unsigned char)buf[used - 1] & 0xC0) == 0x80) used--;
    buf[used] = '\0';
    /* 超限截断标记 */
    if (used >= max_bytes && trunc_mark) {
        size_t tlen = strlen(trunc_mark);
        char *nb = realloc(buf, used + tlen + 1);
        if (nb) {
            buf = nb;
            memcpy(buf + used, trunc_mark, tlen + 1);
        }
    }
    LOG_DEBUG_T("Syscall", "PopenRead", "OK", "used=%zu cap=%zu truncated=%d", used, cap,
                (used >= max_bytes && trunc_mark) ? 1 : 0);
    return buf;
}

/* ============================================================
 * 内部辅助：读取文件内容
 * ============================================================ */
static char* read_file_content(const char *path) {
    LOG_DEBUG_T("Syscall", "ReadFile", "Enter", "path='%s'", path ? path : "(null)");
    if (!path) {
        LOG_ERROR_T("Syscall", "ReadFile", "Invalid", "path is NULL");
        return NULL;
    }

    FILE *fp = fopen(path, "rb");  /* 二进制模式，避免文本转换 */
    if (!fp) {
        LOG_WARN_T("Syscall", "ReadFile", "OpenFail", "fopen %s failed: %s (errno=%d)", path, strerror(errno), errno);
        return NULL;
    }

    fseek(fp, 0, SEEK_END);
    long len = ftell(fp);
    fseek(fp, 0, SEEK_SET);

    if (len < 0) {
        LOG_ERROR_T("Syscall", "ReadFile", "FtellFail", "ftell failed for %s", path);
        fclose(fp);
        return NULL;
    }

    char *buf = malloc(len + 1);
    if (!buf) {
        LOG_ERROR_T("Syscall", "ReadFile", "MallocFail", "malloc(%ld) failed", len);
        fclose(fp);
        return NULL;
    }

    size_t read_len = fread(buf, 1, len, fp);
    fclose(fp);

    if (read_len != (size_t)len) {
        LOG_WARN_T("Syscall", "ReadFile", "ReadPartial", "read %zu of %ld bytes", read_len, len);
    }
    buf[read_len] = '\0';

    LOG_DEBUG_T("Syscall", "ReadFile", "OK", "read %zu bytes from %s", read_len, path);
    return buf;
}

/* ============================================================
 * 内部辅助：写入文件内容
 * ============================================================ */
static int write_file_content(const char *path, const char *content) {
    LOG_DEBUG_T("Syscall", "WriteFile", "Enter", "path='%s', content_len=%zu", path ? path : "(null)", content ? strlen(content) : 0);
    if (!path || !content) {
        LOG_ERROR_T("Syscall", "WriteFile", "Invalid", "path=%p, content=%p", (void*)path, (void*)content);
        return -1;
    }

    FILE *fp = fopen(path, "w");
    if (!fp) {
        LOG_ERROR_T("Syscall", "WriteFile", "OpenFail", "fopen %s failed: %s (errno=%d)", path, strerror(errno), errno);
        return -1;
    }

    size_t len = strlen(content);
    size_t written = fwrite(content, 1, len, fp);
    fclose(fp);

    if (written != len) {
        LOG_ERROR_T("Syscall", "WriteFile", "WriteFail", "wrote %zu of %zu bytes", written, len);
        return -1;
    }

    LOG_DEBUG_T("Syscall", "WriteFile", "OK", "wrote %zu bytes to %s", written, path);
    return 0;
}

/* ============================================================
 * 记忆系统辅助
 * ============================================================ */
#define MEMORY_REGISTRY_PATH "/data/ai_memory/memory_registry.json"

static const char* get_memory_registry_path(void) {
    static char path[512];
    if (path[0] == '\0') {
        const char *root = lingos_data_root();
        safe_snprintf(path, sizeof(path), "%s%s", root, MEMORY_REGISTRY_PATH);
    }
    return path;
}

static void generate_memory_id(char *id, size_t len) {
    time_t t = time(NULL);
    snprintf(id, len, "mem_%ld_%d", t, rand() % 10000);
}

static cJSON* load_memory_registry(void) {
    const char *path = get_memory_registry_path();
    FILE *fp = fopen(path, "r");
    if (!fp) {
        cJSON *root = cJSON_CreateObject();
        cJSON_AddItemToObject(root, "entries", cJSON_CreateArray());
        cJSON_AddStringToObject(root, "version", "1.0");
        return root;
    }
    fseek(fp, 0, SEEK_END);
    long len = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    char *buf = malloc(len + 1);
    if (!buf) { fclose(fp); return NULL; }
    fread(buf, 1, len, fp);
    buf[len] = '\0';
    fclose(fp);
    cJSON *root = cJSON_Parse(buf);
    free(buf);
    if (!root) {
        root = cJSON_CreateObject();
        cJSON_AddItemToObject(root, "entries", cJSON_CreateArray());
        cJSON_AddStringToObject(root, "version", "1.0");
    }
    return root;
}

static int save_memory_registry(cJSON *root) {
    const char *path = get_memory_registry_path();
    char *json_str = cJSON_PrintUnformatted(root);
    if (!json_str) return -1;
    int ret = write_file_content(path, json_str);
    free(json_str);
    return ret;
}

static const char* get_memory_dir(const char *type) {
    static char path[512];
    const char *root = lingos_data_root();
    if (strcmp(type, "short") == 0) {
        safe_snprintf(path, sizeof(path), "%s/data/ai_memory/ai_smemory", root);
    } else if (strcmp(type, "medium") == 0) {
        safe_snprintf(path, sizeof(path), "%s/data/ai_memory/ai_mmemory", root);
    } else {
        safe_snprintf(path, sizeof(path), "%s/data/ai_memory/ai_lmemory", root);
    }
    mkdir(path, 0755);
    return path;
}

/* ============================================================
 * 操作分发（使用 cJSON 序列化，E4 修复）
 * ============================================================ */

/* ============================================================
 * B2: 系统信息辅助函数（CPU/网络/磁盘）
 * ============================================================ */
static unsigned long long syscall_read_cpu_total(int *idle) {
    FILE *fp = fopen("/proc/stat", "r");
    if (!fp) return 0;
    char buf[256] = {0};
    if (!fgets(buf, sizeof(buf), fp)) { fclose(fp); return 0; }
    fclose(fp);
    unsigned long long user = 0, nice = 0, system = 0, idle_ = 0, iowait = 0, irq = 0, softirq = 0, steal = 0;
    if (sscanf(buf, "cpu %llu %llu %llu %llu %llu %llu %llu %llu",
               &user, &nice, &system, &idle_, &iowait, &irq, &softirq, &steal) != 8) return 0;
    if (idle) *idle = (int)(idle_ + iowait);
    return user + nice + system + idle_ + iowait + irq + softirq + steal;
}

static double syscall_read_cpu_usage(void) {
    int idle1 = 0, idle2 = 0;
    unsigned long long t1 = syscall_read_cpu_total(&idle1);
    if (t1 == 0) return 0.0;
    usleep(200000); /* 200ms 采样间隔 */
    unsigned long long t2 = syscall_read_cpu_total(&idle2);
    if (t2 == 0 || t2 <= t1) return 0.0;
    double idle_delta = (double)(idle2 - idle1);
    double total_delta = (double)(t2 - t1);
    if (total_delta <= 0) return 0.0;
    double usage = (1.0 - idle_delta / total_delta) * 100.0;
    return usage < 0 ? 0.0 : usage;
}

static void syscall_read_net_stats(unsigned long long *rx, unsigned long long *tx) {
    *rx = 0; *tx = 0;
    FILE *fp = fopen("/proc/net/dev", "r");
    if (!fp) return;
    char line[512];
    if (!fgets(line, sizeof(line), fp)) { fclose(fp); return; }
    if (!fgets(line, sizeof(line), fp)) { fclose(fp); return; }
    while (fgets(line, sizeof(line), fp)) {
        char iface[64] = {0};
        unsigned long long r = 0, t = 0;
        if (sscanf(line, "%63[^:]: %llu %*u %*u %*u %*u %*u %*u %*u %llu", iface, &r, &t) >= 2) {
            if (strncmp(iface, "lo", 2) == 0) continue; /* 排除回环 */
            *rx += r; *tx += t;
        }
    }
    fclose(fp);
}

static double syscall_read_disk_usage(void) {
    struct statvfs st;
    if (statvfs("/", &st) != 0) return 0.0;
    unsigned long long total = (unsigned long long)st.f_blocks * st.f_frsize;
    unsigned long long free_b = (unsigned long long)st.f_bfree * st.f_frsize;
    if (total == 0) return 0.0;
    return (double)(total - free_b) / (double)total * 100.0;
}

int handle_syscall(const char *operation, const char *args_json, char *out, uint32_t out_len) {
    LOG_DEBUG_T("Syscall", "Handle", "Enter", "operation='%s', args_json='%s'",
                operation ? operation : "(null)", args_json ? args_json : "(null)");

    if (!operation || !args_json || !out) {
        safe_snprintf(out, out_len, "{\"status\":\"error\",\"error_type\":\"invalid_args\",\"message\":\"Invalid parameters\"}");
        LOG_ERROR_T("Syscall", "Handle", "Invalid", "operation=%p, args_json=%p, out=%p", (void*)operation, (void*)args_json, (void*)out);
        return -1;
    }

    cJSON *args = cJSON_Parse(args_json);
    if (!args) {
        safe_snprintf(out, out_len, "{\"status\":\"error\",\"error_type\":\"parse_error\",\"message\":\"Invalid JSON args\"}");
        LOG_ERROR_T("Syscall", "Handle", "ParseFail", "cJSON_Parse failed");
        return -1;
    }

    cJSON *result = cJSON_CreateObject();
    int ret = 0;

    /* ----- 文件操作 ----- */
    if (strcmp(operation, "file_read") == 0) {
        LOG_DEBUG_T("Syscall", "Handle", "FileRead", "reading file");
        cJSON *path_item = cJSON_GetObjectItem(args, "path");
        if (!cJSON_IsString(path_item)) {
            cJSON_AddStringToObject(result, "status", "error");
            cJSON_AddStringToObject(result, "error_type", "missing_param");
            cJSON_AddStringToObject(result, "message", "Missing 'path'");
            ret = -1;
        } else {
            char *data = read_file_content(path_item->valuestring);
            if (data) {
                /* ====== E4 修复：转义控制字符 ====== */
                char *escaped = escape_json_string(data);
                cJSON_AddStringToObject(result, "status", "ok");
                cJSON_AddStringToObject(result, "data", escaped);
                free(escaped);
                free(data);
            } else {
                cJSON_AddStringToObject(result, "status", "error");
                cJSON_AddStringToObject(result, "error_type", "file_not_found");
                cJSON_AddStringToObject(result, "message", strerror(errno));
                ret = -1;
            }
        }
    }
    else if (strcmp(operation, "file_write") == 0) {
        LOG_DEBUG_T("Syscall", "Handle", "FileWrite", "writing file");
        cJSON *path_item = cJSON_GetObjectItem(args, "path");
        cJSON *content_item = cJSON_GetObjectItem(args, "content");
        if (!cJSON_IsString(path_item) || !cJSON_IsString(content_item)) {
            cJSON_AddStringToObject(result, "status", "error");
            cJSON_AddStringToObject(result, "error_type", "missing_param");
            cJSON_AddStringToObject(result, "message", "Missing 'path' or 'content'");
            ret = -1;
        } else {
            if (write_file_content(path_item->valuestring, content_item->valuestring) == 0) {
                cJSON_AddStringToObject(result, "status", "ok");
                cJSON_AddStringToObject(result, "data", "Written");
            } else {
                cJSON_AddStringToObject(result, "status", "error");
                cJSON_AddStringToObject(result, "error_type", "write_failed");
                cJSON_AddStringToObject(result, "message", strerror(errno));
                ret = -1;
            }
        }
    }
    else if (strcmp(operation, "file_delete") == 0) {
        LOG_DEBUG_T("Syscall", "Handle", "FileDelete", "deleting file");
        cJSON *path_item = cJSON_GetObjectItem(args, "path");
        if (!cJSON_IsString(path_item)) {
            cJSON_AddStringToObject(result, "status", "error");
            cJSON_AddStringToObject(result, "error_type", "missing_param");
            cJSON_AddStringToObject(result, "message", "Missing 'path'");
            ret = -1;
        } else {
            if (unlink(path_item->valuestring) == 0) {
                cJSON_AddStringToObject(result, "status", "ok");
                cJSON_AddStringToObject(result, "data", "Deleted");
            } else {
                cJSON_AddStringToObject(result, "status", "error");
                cJSON_AddStringToObject(result, "error_type", "delete_failed");
                cJSON_AddStringToObject(result, "message", strerror(errno));
                ret = -1;
            }
        }
    }
    else if (strcmp(operation, "file_list") == 0) {
        LOG_DEBUG_T("Syscall", "Handle", "FileList", "listing directory");
        cJSON *path_item = cJSON_GetObjectItem(args, "path");
        const char *path = (path_item && cJSON_IsString(path_item)) ? path_item->valuestring : ".";
        DIR *d = opendir(path);
        if (d) {
            cJSON *files = cJSON_CreateArray();
            struct dirent *entry;
            while ((entry = readdir(d)) != NULL) {
                if (entry->d_name[0] == '.') continue;
                cJSON_AddItemToArray(files, cJSON_CreateString(entry->d_name));
            }
            closedir(d);
            cJSON_AddStringToObject(result, "status", "ok");
            cJSON_AddItemToObject(result, "data", files);
        } else {
            cJSON_AddStringToObject(result, "status", "error");
            cJSON_AddStringToObject(result, "error_type", "opendir_failed");
            cJSON_AddStringToObject(result, "message", strerror(errno));
            ret = -1;
        }
    }
    else if (strcmp(operation, "file_mkdir") == 0) {
        LOG_DEBUG_T("Syscall", "Handle", "FileMkdir", "creating directory");
        cJSON *path_item = cJSON_GetObjectItem(args, "path");
        if (!cJSON_IsString(path_item)) {
            cJSON_AddStringToObject(result, "status", "error");
            cJSON_AddStringToObject(result, "error_type", "missing_param");
            cJSON_AddStringToObject(result, "message", "Missing 'path'");
            ret = -1;
        } else {
            if (mkdir(path_item->valuestring, 0755) == 0) {
                cJSON_AddStringToObject(result, "status", "ok");
                cJSON_AddStringToObject(result, "data", "Created");
            } else {
                cJSON_AddStringToObject(result, "status", "error");
                cJSON_AddStringToObject(result, "error_type", "mkdir_failed");
                cJSON_AddStringToObject(result, "message", strerror(errno));
                ret = -1;
            }
        }
    }

    /* ----- 进程操作 ----- */
    else if (strcmp(operation, "process_list") == 0) {
        LOG_DEBUG_T("Syscall", "Handle", "ProcessList", "listing processes");
        FILE *fp = popen("ps aux", "r");
        if (fp) {
            /* 【修复】动态读取（256KB 上限） */
            char *buf = popen_read_all(fp, 256 * 1024, "\n...[ps output truncated]");
            pclose(fp);
            if (buf) {
                char *escaped = escape_json_string(buf);
                cJSON_AddStringToObject(result, "status", "ok");
                cJSON_AddStringToObject(result, "data", escaped);
                free(escaped);
                free(buf);
            } else {
                cJSON_AddStringToObject(result, "status", "error");
                cJSON_AddStringToObject(result, "error_type", "alloc_failed");
                cJSON_AddStringToObject(result, "message", "Failed to read ps output");
                ret = -1;
            }
        } else {
            cJSON_AddStringToObject(result, "status", "error");
            cJSON_AddStringToObject(result, "error_type", "popen_failed");
            cJSON_AddStringToObject(result, "message", "popen failed");
            ret = -1;
        }
    }
    else if (strcmp(operation, "process_kill") == 0) {
        LOG_DEBUG_T("Syscall", "Handle", "ProcessKill", "killing process");
        cJSON *pid_item = cJSON_GetObjectItem(args, "pid");
        if (!cJSON_IsNumber(pid_item)) {
            cJSON_AddStringToObject(result, "status", "error");
            cJSON_AddStringToObject(result, "error_type", "missing_param");
            cJSON_AddStringToObject(result, "message", "Missing 'pid'");
            ret = -1;
        } else {
            if (kill(pid_item->valueint, SIGTERM) == 0) {
                cJSON_AddStringToObject(result, "status", "ok");
                cJSON_AddStringToObject(result, "data", "Killed");
            } else {
                cJSON_AddStringToObject(result, "status", "error");
                cJSON_AddStringToObject(result, "error_type", "kill_failed");
                cJSON_AddStringToObject(result, "message", strerror(errno));
                ret = -1;
            }
        }
    }
    else if (strcmp(operation, "process_info") == 0) {
        LOG_DEBUG_T("Syscall", "Handle", "ProcessInfo", "getting process info");
        cJSON *pid_item = cJSON_GetObjectItem(args, "pid");
        if (!cJSON_IsNumber(pid_item)) {
            cJSON_AddStringToObject(result, "status", "error");
            cJSON_AddStringToObject(result, "error_type", "missing_param");
            cJSON_AddStringToObject(result, "message", "Missing 'pid'");
            ret = -1;
        } else {
            if (kill(pid_item->valueint, 0) == 0) {
                cJSON_AddStringToObject(result, "status", "ok");
                cJSON_AddStringToObject(result, "data", "Running");
            } else {
                cJSON_AddStringToObject(result, "status", "error");
                cJSON_AddStringToObject(result, "error_type", "process_not_found");
                cJSON_AddStringToObject(result, "message", "Process not found");
                ret = -1;
            }
        }
    }

    /* ----- 系统信息 ----- */
    else if (strcmp(operation, "system_info") == 0) {
        LOG_DEBUG_T("Syscall", "Handle", "SystemInfo", "getting system info");
        struct sysinfo info;
        if (sysinfo(&info) == 0) {
            cJSON *data = cJSON_CreateObject();
            cJSON_AddNumberToObject(data, "uptime", info.uptime);
            cJSON_AddNumberToObject(data, "total_ram", info.totalram);
            cJSON_AddNumberToObject(data, "free_ram", info.freeram);
            cJSON_AddNumberToObject(data, "load_avg_1", info.loads[0]);
            cJSON_AddNumberToObject(data, "load_avg_5", info.loads[1]);
            cJSON_AddNumberToObject(data, "load_avg_15", info.loads[2]);
            /* B2: 增强字段（CPU 使用率/网络收发/磁盘使用率） */
            cJSON_AddNumberToObject(data, "cpu_usage", syscall_read_cpu_usage());
            unsigned long long net_rx = 0, net_tx = 0;
            syscall_read_net_stats(&net_rx, &net_tx);
            cJSON_AddNumberToObject(data, "network_rx", (double)net_rx);
            cJSON_AddNumberToObject(data, "network_tx", (double)net_tx);
            cJSON_AddNumberToObject(data, "disk_usage", syscall_read_disk_usage());
            cJSON_AddStringToObject(result, "status", "ok");
            cJSON_AddItemToObject(result, "data", data);
        } else {
            cJSON_AddStringToObject(result, "status", "error");
            cJSON_AddStringToObject(result, "error_type", "sysinfo_failed");
            cJSON_AddStringToObject(result, "message", strerror(errno));
            ret = -1;
        }
    }
    else if (strcmp(operation, "system_uptime") == 0) {
        LOG_DEBUG_T("Syscall", "Handle", "SystemUptime", "getting uptime");
        struct sysinfo info;
        if (sysinfo(&info) == 0) {
            char buf[64];
            safe_snprintf(buf, sizeof(buf), "%ld seconds", info.uptime);
            cJSON_AddStringToObject(result, "status", "ok");
            cJSON_AddStringToObject(result, "data", buf);
        } else {
            cJSON_AddStringToObject(result, "status", "error");
            cJSON_AddStringToObject(result, "error_type", "sysinfo_failed");
            cJSON_AddStringToObject(result, "message", strerror(errno));
            ret = -1;
        }
    }
    else if (strcmp(operation, "system_memory") == 0) {
        LOG_DEBUG_T("Syscall", "Handle", "SystemMemory", "getting memory info");
        struct sysinfo info;
        if (sysinfo(&info) == 0) {
            char buf[128];
            safe_snprintf(buf, sizeof(buf), "Total: %ld MB, Free: %ld MB",
                          info.totalram / (1024*1024), info.freeram / (1024*1024));
            cJSON_AddStringToObject(result, "status", "ok");
            cJSON_AddStringToObject(result, "data", buf);
        } else {
            cJSON_AddStringToObject(result, "status", "error");
            cJSON_AddStringToObject(result, "error_type", "sysinfo_failed");
            cJSON_AddStringToObject(result, "message", strerror(errno));
            ret = -1;
        }
    }
    else if (strcmp(operation, "system_disk") == 0) {
        LOG_DEBUG_T("Syscall", "Handle", "SystemDisk", "getting disk info");
        FILE *fp = popen("df -h /", "r");
        if (fp) {
            char buf[512] = {0};
            fread(buf, 1, sizeof(buf)-1, fp);
            pclose(fp);
            char *escaped = escape_json_string(buf);
            cJSON_AddStringToObject(result, "status", "ok");
            cJSON_AddStringToObject(result, "data", escaped);
            free(escaped);
        } else {
            cJSON_AddStringToObject(result, "status", "error");
            cJSON_AddStringToObject(result, "error_type", "popen_failed");
            cJSON_AddStringToObject(result, "message", "popen failed");
            ret = -1;
        }
    }
    else if (strcmp(operation, "system_cpu") == 0) {
        LOG_DEBUG_T("Syscall", "Handle", "SystemCPU", "getting CPU info");
        FILE *fp = fopen("/proc/stat", "r");
        if (fp) {
            char buf[256] = {0};
            fgets(buf, sizeof(buf), fp);
            fclose(fp);
            char *escaped = escape_json_string(buf);
            cJSON_AddStringToObject(result, "status", "ok");
            cJSON_AddStringToObject(result, "data", escaped);
            free(escaped);
        } else {
            cJSON_AddStringToObject(result, "status", "error");
            cJSON_AddStringToObject(result, "error_type", "proc_stat_failed");
            cJSON_AddStringToObject(result, "message", strerror(errno));
            ret = -1;
        }
    }

    /* ----- 网络操作 ----- */
    else if (strcmp(operation, "net_ping") == 0) {
        LOG_DEBUG_T("Syscall", "Handle", "NetPing", "ping");
        cJSON *host_item = cJSON_GetObjectItem(args, "host");
        cJSON *count_item = cJSON_GetObjectItem(args, "count");
        const char *host = (host_item && cJSON_IsString(host_item)) ? host_item->valuestring : "8.8.8.8";
        int count = (count_item && cJSON_IsNumber(count_item)) ? count_item->valueint : 1;
        char cmd[128];
        safe_snprintf(cmd, sizeof(cmd), "ping -c %d %s 2>&1", count, host);
        FILE *fp = popen(cmd, "r");
        if (fp) {
            char buf[1024] = {0};
            fread(buf, 1, sizeof(buf)-1, fp);
            pclose(fp);
            char *escaped = escape_json_string(buf);
            cJSON_AddStringToObject(result, "status", "ok");
            cJSON_AddStringToObject(result, "data", escaped);
            free(escaped);
        } else {
            cJSON_AddStringToObject(result, "status", "error");
            cJSON_AddStringToObject(result, "error_type", "popen_failed");
            cJSON_AddStringToObject(result, "message", "popen failed");
            ret = -1;
        }
    }
    else if (strcmp(operation, "net_curl") == 0) {
        LOG_DEBUG_T("Syscall", "Handle", "NetCurl", "HTTP request");
        cJSON *url_item = cJSON_GetObjectItem(args, "url");
        if (!cJSON_IsString(url_item)) {
            cJSON_AddStringToObject(result, "status", "error");
            cJSON_AddStringToObject(result, "error_type", "missing_param");
            cJSON_AddStringToObject(result, "message", "Missing 'url'");
            ret = -1;
        } else {
            char cmd[256];
            safe_snprintf(cmd, sizeof(cmd), "curl -s -m 5 '%s' 2>&1", url_item->valuestring);
            FILE *fp = popen(cmd, "r");
            if (fp) {
                /* 【修复】动态读取全量响应（1MB 上限 + 截断标记） */
                char *buf = popen_read_all(fp, 1024 * 1024, "\n...[response truncated at 1MB]");
                pclose(fp);
                if (buf) {
                    char *escaped = escape_json_string(buf);
                    cJSON_AddStringToObject(result, "status", "ok");
                    cJSON_AddStringToObject(result, "data", escaped);
                    free(escaped);
                    free(buf);
                } else {
                    cJSON_AddStringToObject(result, "status", "error");
                    cJSON_AddStringToObject(result, "error_type", "alloc_failed");
                    cJSON_AddStringToObject(result, "message", "Failed to read HTTP response");
                    ret = -1;
                }
            } else {
                cJSON_AddStringToObject(result, "status", "error");
                cJSON_AddStringToObject(result, "error_type", "popen_failed");
                cJSON_AddStringToObject(result, "message", "popen failed");
                ret = -1;
            }
        }
    }
    else if (strcmp(operation, "net_dns_lookup") == 0) {
        LOG_DEBUG_T("Syscall", "Handle", "NetDNS", "DNS lookup");
        cJSON *domain_item = cJSON_GetObjectItem(args, "domain");
        if (!cJSON_IsString(domain_item)) {
            cJSON_AddStringToObject(result, "status", "error");
            cJSON_AddStringToObject(result, "error_type", "missing_param");
            cJSON_AddStringToObject(result, "message", "Missing 'domain'");
            ret = -1;
        } else {
            struct addrinfo hints, *res;
            memset(&hints, 0, sizeof(hints));
            hints.ai_family = AF_UNSPEC;
            if (getaddrinfo(domain_item->valuestring, NULL, &hints, &res) == 0) {
                char ip[INET6_ADDRSTRLEN];
                void *addr;
                if (res->ai_family == AF_INET) {
                    addr = &((struct sockaddr_in *)res->ai_addr)->sin_addr;
                } else {
                    addr = &((struct sockaddr_in6 *)res->ai_addr)->sin6_addr;
                }
                inet_ntop(res->ai_family, addr, ip, sizeof(ip));
                freeaddrinfo(res);
                cJSON_AddStringToObject(result, "status", "ok");
                cJSON_AddStringToObject(result, "data", ip);
            } else {
                cJSON_AddStringToObject(result, "status", "error");
                cJSON_AddStringToObject(result, "error_type", "dns_failed");
                cJSON_AddStringToObject(result, "message", "DNS lookup failed");
                ret = -1;
            }
        }
    }
    else if (strcmp(operation, "exec_command") == 0) {
        LOG_DEBUG_T("Syscall", "Handle", "ExecCommand", "executing command");
        cJSON *cmd_item = cJSON_GetObjectItem(args, "command");
        if (!cJSON_IsString(cmd_item)) {
            cJSON_AddStringToObject(result, "status", "error");
            cJSON_AddStringToObject(result, "error_type", "missing_param");
            cJSON_AddStringToObject(result, "message", "Missing 'command'");
            ret = -1;
        } else {
            FILE *fp = popen(cmd_item->valuestring, "r");
            if (fp) {
                /* 【修复】动态读取全量输出（1MB 上限 + 截断标记） */
                char *buf = popen_read_all(fp, 1024 * 1024, "\n...[output truncated at 1MB]");
                pclose(fp);
                if (buf) {
                    char *escaped = escape_json_string(buf);
                    cJSON_AddStringToObject(result, "status", "ok");
                    cJSON_AddStringToObject(result, "data", escaped);
                    free(escaped);
                    free(buf);
                } else {
                    cJSON_AddStringToObject(result, "status", "error");
                    cJSON_AddStringToObject(result, "error_type", "alloc_failed");
                    cJSON_AddStringToObject(result, "message", "Failed to read command output");
                    ret = -1;
                }
            } else {
                cJSON_AddStringToObject(result, "status", "error");
                cJSON_AddStringToObject(result, "error_type", "popen_failed");
                cJSON_AddStringToObject(result, "message", strerror(errno));
                ret = -1;
            }
        }
    }

    /* ====== 记忆原子操作 ====== */
    else if (strcmp(operation, "memory_write") == 0) {
        LOG_DEBUG_T("Syscall", "Handle", "MemoryWrite", "writing memory");
        cJSON *type_item = cJSON_GetObjectItem(args, "type");
        cJSON *content_item = cJSON_GetObjectItem(args, "content");
        cJSON *keywords_item = cJSON_GetObjectItem(args, "keywords");
        if (!cJSON_IsString(type_item) || !cJSON_IsString(content_item)) {
            cJSON_AddStringToObject(result, "status", "error");
            cJSON_AddStringToObject(result, "error_type", "missing_param");
            cJSON_AddStringToObject(result, "message", "Missing 'type' or 'content'");
            ret = -1;
        } else {
            const char *type = type_item->valuestring;
            if (strcmp(type, "short") != 0 && strcmp(type, "medium") != 0 && strcmp(type, "long") != 0) {
                cJSON_AddStringToObject(result, "status", "error");
                cJSON_AddStringToObject(result, "error_type", "invalid_param");
                cJSON_AddStringToObject(result, "message", "type must be 'short', 'medium', or 'long'");
                ret = -1;
            } else {
                char id[64];
                generate_memory_id(id, sizeof(id));
                const char *dir = get_memory_dir(type);
                char file_path[512];
                safe_snprintf(file_path, sizeof(file_path), "%s/%s.json", dir, id);

                cJSON *mem_obj = cJSON_CreateObject();
                cJSON_AddStringToObject(mem_obj, "id", id);
                cJSON_AddStringToObject(mem_obj, "type", type);
                cJSON_AddStringToObject(mem_obj, "content", content_item->valuestring);
                cJSON_AddNumberToObject(mem_obj, "timestamp", (double)time(NULL));

                cJSON *kw_array = cJSON_CreateArray();
                if (keywords_item && cJSON_IsArray(keywords_item)) {
                    int size = cJSON_GetArraySize(keywords_item);
                    for (int i = 0; i < size; i++) {
                        cJSON *kw = cJSON_GetArrayItem(keywords_item, i);
                        if (cJSON_IsString(kw)) {
                            cJSON_AddItemToArray(kw_array, cJSON_CreateString(kw->valuestring));
                        }
                    }
                } else {
                    cJSON_AddItemToArray(kw_array, cJSON_CreateString("default"));
                }
                cJSON_AddItemToObject(mem_obj, "keywords", kw_array);

                char *json_str = cJSON_PrintUnformatted(mem_obj);
                cJSON_Delete(mem_obj);

                if (!json_str) {
                    cJSON_AddStringToObject(result, "status", "error");
                    cJSON_AddStringToObject(result, "error_type", "json_error");
                    cJSON_AddStringToObject(result, "message", "Failed to serialize memory");
                    ret = -1;
                } else {
                    if (write_file_content(file_path, json_str) == 0) {
                        cJSON *registry = load_memory_registry();
                        if (registry) {
                            cJSON *entries = cJSON_GetObjectItem(registry, "entries");
                            if (entries) {
                                cJSON *entry = cJSON_CreateObject();
                                cJSON_AddStringToObject(entry, "id", id);
                                cJSON_AddStringToObject(entry, "type", type);
                                cJSON_AddStringToObject(entry, "summary", content_item->valuestring);
                                cJSON *kw_list = cJSON_CreateArray();
                                if (keywords_item && cJSON_IsArray(keywords_item)) {
                                    int size = cJSON_GetArraySize(keywords_item);
                                    for (int i = 0; i < size && i < 3; i++) {
                                        cJSON *kw = cJSON_GetArrayItem(keywords_item, i);
                                        if (cJSON_IsString(kw)) {
                                            cJSON_AddItemToArray(kw_list, cJSON_CreateString(kw->valuestring));
                                        }
                                    }
                                }
                                if (cJSON_GetArraySize(kw_list) == 0) {
                                    cJSON_AddItemToArray(kw_list, cJSON_CreateString("default"));
                                }
                                cJSON_AddItemToObject(entry, "keywords", kw_list);
                                cJSON_AddNumberToObject(entry, "timestamp", (double)time(NULL));
                                cJSON_AddItemToArray(entries, entry);
                            }
                            save_memory_registry(registry);
                            cJSON_Delete(registry);
                        }
                        cJSON_AddStringToObject(result, "status", "ok");
                        cJSON_AddStringToObject(result, "data", id);
                    } else {
                        cJSON_AddStringToObject(result, "status", "error");
                        cJSON_AddStringToObject(result, "error_type", "write_failed");
                        cJSON_AddStringToObject(result, "message", "Failed to write memory file");
                        ret = -1;
                    }
                    free(json_str);
                }
            }
        }
    }
    else if (strcmp(operation, "memory_search") == 0) {
        LOG_DEBUG_T("Syscall", "Handle", "MemorySearch", "searching memory");
        cJSON *keyword_item = cJSON_GetObjectItem(args, "keyword");
        if (!cJSON_IsString(keyword_item)) {
            cJSON_AddStringToObject(result, "status", "error");
            cJSON_AddStringToObject(result, "error_type", "missing_param");
            cJSON_AddStringToObject(result, "message", "Missing 'keyword'");
            ret = -1;
        } else {
            const char *keyword = keyword_item->valuestring;
            cJSON *registry = load_memory_registry();
            cJSON *entries = cJSON_GetObjectItem(registry, "entries");
            cJSON *matches = cJSON_CreateArray();
            if (entries && cJSON_IsArray(entries)) {
                int size = cJSON_GetArraySize(entries);
                for (int i = 0; i < size; i++) {
                    cJSON *entry = cJSON_GetArrayItem(entries, i);
                    cJSON *kw_list = cJSON_GetObjectItem(entry, "keywords");
                    int found = 0;
                    if (kw_list && cJSON_IsArray(kw_list)) {
                        int kw_size = cJSON_GetArraySize(kw_list);
                        for (int j = 0; j < kw_size; j++) {
                            cJSON *kw = cJSON_GetArrayItem(kw_list, j);
                            if (cJSON_IsString(kw) && strstr(kw->valuestring, keyword) != NULL) {
                                found = 1;
                                break;
                            }
                        }
                    }
                    cJSON *summary = cJSON_GetObjectItem(entry, "summary");
                    if (!found && cJSON_IsString(summary) && strstr(summary->valuestring, keyword) != NULL) {
                        found = 1;
                    }
                    if (found) {
                        cJSON_AddItemToArray(matches, cJSON_Duplicate(entry, 1));
                    }
                }
            }
            cJSON_AddStringToObject(result, "status", "ok");
            cJSON_AddItemToObject(result, "data", matches);
            cJSON_Delete(registry);
        }
    }
    else if (strcmp(operation, "memory_read") == 0) {
        LOG_DEBUG_T("Syscall", "Handle", "MemoryRead", "reading memory");
        cJSON *id_item = cJSON_GetObjectItem(args, "id");
        if (!cJSON_IsString(id_item)) {
            cJSON_AddStringToObject(result, "status", "error");
            cJSON_AddStringToObject(result, "error_type", "missing_param");
            cJSON_AddStringToObject(result, "message", "Missing 'id'");
            ret = -1;
        } else {
            const char *id = id_item->valuestring;
            cJSON *registry = load_memory_registry();
            cJSON *entries = cJSON_GetObjectItem(registry, "entries");
            const char *found_type = NULL;
            if (entries && cJSON_IsArray(entries)) {
                int size = cJSON_GetArraySize(entries);
                for (int i = 0; i < size; i++) {
                    cJSON *entry = cJSON_GetArrayItem(entries, i);
                    cJSON *eid = cJSON_GetObjectItem(entry, "id");
                    if (cJSON_IsString(eid) && strcmp(eid->valuestring, id) == 0) {
                        cJSON *type = cJSON_GetObjectItem(entry, "type");
                        if (cJSON_IsString(type)) {
                            found_type = type->valuestring;
                        }
                        break;
                    }
                }
            }
            cJSON_Delete(registry);
            if (!found_type) {
                cJSON_AddStringToObject(result, "status", "error");
                cJSON_AddStringToObject(result, "error_type", "not_found");
                cJSON_AddStringToObject(result, "message", "Memory ID not found");
                ret = -1;
            } else {
                const char *dir = get_memory_dir(found_type);
                char file_path[512];
                safe_snprintf(file_path, sizeof(file_path), "%s/%s.json", dir, id);
                char *content = read_file_content(file_path);
                if (content) {
                    cJSON_AddStringToObject(result, "status", "ok");
                    cJSON_AddStringToObject(result, "data", content);
                    free(content);
                } else {
                    cJSON_AddStringToObject(result, "status", "error");
                    cJSON_AddStringToObject(result, "error_type", "read_failed");
                    cJSON_AddStringToObject(result, "message", "Failed to read memory file");
                    ret = -1;
                }
            }
        }
    }
    else if (strcmp(operation, "memory_delete") == 0) {
        LOG_DEBUG_T("Syscall", "Handle", "MemoryDelete", "deleting memory");
        cJSON *id_item = cJSON_GetObjectItem(args, "id");
        if (!cJSON_IsString(id_item)) {
            cJSON_AddStringToObject(result, "status", "error");
            cJSON_AddStringToObject(result, "error_type", "missing_param");
            cJSON_AddStringToObject(result, "message", "Missing 'id'");
            ret = -1;
        } else {
            const char *id = id_item->valuestring;
            cJSON *registry = load_memory_registry();
            cJSON *entries = cJSON_GetObjectItem(registry, "entries");
            int found = 0;
            if (entries && cJSON_IsArray(entries)) {
                int size = cJSON_GetArraySize(entries);
                for (int i = 0; i < size; i++) {
                    cJSON *entry = cJSON_GetArrayItem(entries, i);
                    cJSON *eid = cJSON_GetObjectItem(entry, "id");
                    if (cJSON_IsString(eid) && strcmp(eid->valuestring, id) == 0) {
                        cJSON_DeleteItemFromArray(entries, i);
                        found = 1;
                        break;
                    }
                }
            }
            if (found) {
                save_memory_registry(registry);
                const char *types[] = {"short", "medium", "long"};
                int deleted = 0;
                for (int t = 0; t < 3; t++) {
                    const char *dir = get_memory_dir(types[t]);
                    char file_path[512];
                    safe_snprintf(file_path, sizeof(file_path), "%s/%s.json", dir, id);
                    if (unlink(file_path) == 0) {
                        deleted = 1;
                        break;
                    }
                }
                cJSON_Delete(registry);
                if (deleted) {
                    cJSON_AddStringToObject(result, "status", "ok");
                    cJSON_AddStringToObject(result, "data", "Deleted");
                } else {
                    cJSON_AddStringToObject(result, "status", "ok");
                    cJSON_AddStringToObject(result, "data", "Registry entry removed, but file not found");
                }
            } else {
                cJSON_Delete(registry);
                cJSON_AddStringToObject(result, "status", "error");
                cJSON_AddStringToObject(result, "error_type", "not_found");
                cJSON_AddStringToObject(result, "message", "Memory ID not found in registry");
                ret = -1;
            }
        }
    }
    else if (strcmp(operation, "memory_index") == 0) {
        LOG_DEBUG_T("Syscall", "Handle", "MemoryIndex", "listing memory index");
        cJSON *type_item = cJSON_GetObjectItem(args, "type");
        const char *type = (type_item && cJSON_IsString(type_item)) ? type_item->valuestring : NULL;
        cJSON *registry = load_memory_registry();
        cJSON *entries = cJSON_GetObjectItem(registry, "entries");
        cJSON *result_list = cJSON_CreateArray();
        if (entries && cJSON_IsArray(entries)) {
            int size = cJSON_GetArraySize(entries);
            for (int i = 0; i < size; i++) {
                cJSON *entry = cJSON_GetArrayItem(entries, i);
                if (type) {
                    cJSON *etyp = cJSON_GetObjectItem(entry, "type");
                    if (!cJSON_IsString(etyp) || strcmp(etyp->valuestring, type) != 0) {
                        continue;
                    }
                }
                cJSON_AddItemToArray(result_list, cJSON_Duplicate(entry, 1));
            }
        }
        cJSON_AddStringToObject(result, "status", "ok");
        cJSON_AddItemToObject(result, "data", result_list);
        cJSON_Delete(registry);
    }

    /* ----- 未知操作 ----- */
    else {
        LOG_WARN_T("Syscall", "Handle", "Unknown", "unknown operation: %s", operation);
        cJSON_AddStringToObject(result, "status", "error");
        cJSON_AddStringToObject(result, "error_type", "unknown_operation");
        cJSON_AddStringToObject(result, "message", "Unknown operation");
        ret = -1;
    }

    cJSON_Delete(args);

    /* 序列化响应（使用 cJSON，确保 JSON 格式正确） */
    char *json_str = cJSON_PrintUnformatted(result);
    cJSON_Delete(result);

    if (json_str) {
        safe_strncpy(out, json_str, out_len);
        out[out_len - 1] = '\0';
        free(json_str);
        LOG_DEBUG_T("Syscall", "Handle", "OK", "operation=%s ret=%d", operation, ret);
        return ret;
    } else {
        LOG_ERROR_T("Syscall", "Handle", "SerializeFail", "cJSON_PrintUnformatted failed");
        safe_snprintf(out, out_len, "{\"status\":\"error\",\"error_type\":\"json_error\",\"message\":\"JSON serialization failed\"}");
        return -1;
    }
}