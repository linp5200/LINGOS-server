/**
 * @file    src/lib/log_extra.c
 * @brief   分级日志系统（彩色终端 + 按天轮转 + 启动序号）
 * @version LN-B-5.1.2.6-rc
 * @par     核心协议：C1, C-C, AI-CTL
 * @changes 增强 log_draw_progress_full 支持速度/大小/耗时；
 *          修复 log_draw_status_bar 空指针检查 + 行清除 + 换行；
 *          保持原有 API 兼容。
 */

#include "log_extra.h"
#include "../common/data_path.h"
#include "../common/lang.h"
#include "../common/safe_string.h"
#include "../drivers/uart.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/stat.h>
#include <errno.h>
#include <stdarg.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <sys/wait.h>
#include <signal.h>
#include <dirent.h>

#define LOG_DIR "/log"
#define LOG_FILE_PREFIX "lingos"
#define MAX_LOG_PATH 512
#define MAX_LOG_MSG 8192
#define MAX_MODULES 32
#define SEQ_FILE "/LINGOS/state/startup_seq"

typedef struct {
    char name[32];
    int level;
} module_level_t;

static module_level_t module_levels[MAX_MODULES];
static int module_count = 0;
static pthread_mutex_t module_lock = PTHREAD_MUTEX_INITIALIZER;

static int current_log_level = LOG_LEVEL_WARN;  /* 【修复】运行时默认 WARNING */
static int g_global_level = LOG_LEVEL_WARN;  /* 【修复】全局默认 WARNING */
static int console_output = 1;
static int file_output = 1;                 /* 【2026-08-22 定稿】文件保存开关：默认开=DEBUG 全量；关=仅 WARN+ */
static long log_seq = 0;                    /* 【2026-08-22 定稿】JSON 文件 id 自增序号（进程内） */
static int initialized = 0;
static char current_log_path[MAX_LOG_PATH] = {0};
static char current_date[11] = {0};
static int current_seq = 0;

static pthread_t cleanup_thread;
static volatile int cleanup_thread_running = 0;
static volatile int cleanup_stop_flag = 0;

/* ============================================================
 * 紧急写入
 * ============================================================ */
void emergency_write(const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    vfprintf(stderr, fmt, args);
    fflush(stderr);
    va_end(args);
}

static void emergency_write_simple(const char *msg) {
    fputs(msg, stderr);
    fflush(stderr);
}

/* ============================================================
 * 日志目录与文件管理
 * ============================================================ */
static void get_date_str(char *buf, size_t size) {
    time_t t = time(NULL);
    struct tm *tm = localtime(&t);
    strftime(buf, size, "%Y%m%d", tm);
}

static int read_startup_seq(void) {
    FILE *fp = fopen(SEQ_FILE, "r");
    if (!fp) {
        emergency_write_simple("[LogExtra] SEQ file not found, starting with 0\n");
        return 0;
    }
    int seq = 0;
    if (fscanf(fp, "%d", &seq) != 1) {
        seq = 0;
    }
    fclose(fp);
    emergency_write_simple("[LogExtra] Read startup seq: ");
    char buf[16];
    safe_snprintf(buf, sizeof(buf), "%d\n", seq);
    emergency_write_simple(buf);
    return seq;
}

static void write_startup_seq(int seq) {
    char dir[512];
    const char *root = lingos_data_root();
    safe_snprintf(dir, sizeof(dir), "%s/state", root);
    if (access(dir, F_OK) != 0) {
        if (mkdir(dir, 0755) != 0) {
            emergency_write_simple("[LogExtra] Failed to create state dir\n");
            return;
        }
    }
    FILE *fp = fopen(SEQ_FILE, "w");
    if (!fp) {
        emergency_write_simple("[LogExtra] Failed to write seq file\n");
        return;
    }
    fprintf(fp, "%d\n", seq);
    fclose(fp);
}

static int increment_startup_seq(void) {
    int seq = read_startup_seq();
    seq++;
    write_startup_seq(seq);
    return seq;
}

static void ensure_log_dir(void) {
    const char *root = lingos_data_root();
    char dir[MAX_LOG_PATH];
    safe_snprintf(dir, sizeof(dir), "%s%s", root, LOG_DIR);
    if (access(dir, F_OK) != 0) {
        if (mkdir(dir, 0755) != 0 && errno != EEXIST) {
            emergency_write_simple("[LogExtra] Failed to create log directory\n");
        }
    }
}

static void update_log_path(void) {
    /* 【2026-08-22 定稿】单文件：/log/lingos.log（不再按日期+序号分文件——
     * 修复"序号跨日期不重置"与多文件碎片问题） */
    if (current_log_path[0] == '\0') {
        const char *root = lingos_data_root();
        safe_snprintf(current_log_path, sizeof(current_log_path),
                      "%s%s/%s.log", root, LOG_DIR, LOG_FILE_PREFIX);
        ensure_log_dir();

        char msg[256];
        safe_snprintf(msg, sizeof(msg), "[LogExtra] Log file: %s\n", current_log_path);
        emergency_write_simple(msg);
    }
}

/* ============================================================
 * 日志清理
 * ============================================================ */
static void cleanup_old_logs(void) {
    const char *root = lingos_data_root();
    char log_dir[MAX_LOG_PATH];
    safe_snprintf(log_dir, sizeof(log_dir), "%s%s", root, LOG_DIR);

    DIR *d = opendir(log_dir);
    if (!d) {
        emergency_write_simple("[LogExtra] Cannot open log dir for cleanup\n");
        return;
    }

    time_t now = time(NULL);
    time_t cutoff = now - 86400;

    struct dirent *entry;
    int deleted = 0;
    while ((entry = readdir(d)) != NULL) {
        if (entry->d_name[0] == '.') continue;
        if (strncmp(entry->d_name, LOG_FILE_PREFIX, strlen(LOG_FILE_PREFIX)) != 0) continue;
        /* 【2026-08-22 定稿】单文件 lingos.log 永不清理（活跃日志）；只清旧多文件 */
        if (strcmp(entry->d_name, LOG_FILE_PREFIX ".log") == 0) continue;

        char full_path[MAX_LOG_PATH];
        safe_snprintf(full_path, sizeof(full_path), "%s/%s", log_dir, entry->d_name);

        struct stat st;
        if (stat(full_path, &st) != 0) continue;
        if (!S_ISREG(st.st_mode)) continue;

        if (st.st_mtime < cutoff) {
            if (unlink(full_path) == 0) {
                deleted++;
                char msg[128];
                safe_snprintf(msg, sizeof(msg), "[LogExtra] Deleted old log: %s\n", entry->d_name);
                emergency_write_simple(msg);
            }
        }
    }
    closedir(d);
    if (deleted > 0) {
        char msg[64];
        safe_snprintf(msg, sizeof(msg), "[LogExtra] Cleaned up %d old logs\n", deleted);
        emergency_write_simple(msg);
    }
}

static void* cleanup_thread_func(void *arg) {
    (void)arg;
    emergency_write_simple("[LogExtra] Cleanup thread started\n");
    cleanup_thread_running = 1;

    while (!cleanup_stop_flag) {
        time_t now = time(NULL);
        struct tm *tm = localtime(&now);
        struct tm next_midnight = *tm;
        next_midnight.tm_hour = 0;
        next_midnight.tm_min = 0;
        next_midnight.tm_sec = 0;
        next_midnight.tm_mday += 1;
        time_t next = mktime(&next_midnight);
        long sleep_sec = (long)(next - now);
        if (sleep_sec < 0) sleep_sec = 0;

        char msg[64];
        safe_snprintf(msg, sizeof(msg), "[LogExtra] Next cleanup in %ld seconds\n", sleep_sec);
        emergency_write_simple(msg);

        while (sleep_sec > 0 && !cleanup_stop_flag) {
            int step = (sleep_sec > 60) ? 60 : (int)sleep_sec;
            sleep(step);
            sleep_sec -= step;
        }

        if (cleanup_stop_flag) break;

        emergency_write_simple("[LogExtra] Running scheduled log cleanup\n");
        cleanup_old_logs();
    }

    cleanup_thread_running = 0;
    emergency_write_simple("[LogExtra] Cleanup thread stopped\n");
    return NULL;
}

static void log_cleanup_atexit(void) {
    emergency_write_simple("[LogExtra] atexit: stopping cleanup thread\n");
    cleanup_stop_flag = 1;
    if (cleanup_thread_running) {
        int wait_count = 0;
        while (cleanup_thread_running && wait_count < 30) {
            usleep(100000);
            wait_count++;
        }
    }
    emergency_write_simple("[LogExtra] atexit: cleanup thread stopped\n");
}

static void start_cleanup_thread(void) {
    if (cleanup_thread_running) {
        emergency_write_simple("[LogExtra] Cleanup thread already running\n");
        return;
    }
    cleanup_stop_flag = 0;
    if (pthread_create(&cleanup_thread, NULL, cleanup_thread_func, NULL) != 0) {
        emergency_write_simple("[LogExtra] Failed to start cleanup thread\n");
        return;
    }
    atexit(log_cleanup_atexit);
    emergency_write_simple("[LogExtra] Cleanup thread started\n");
}

/* ============================================================
 * 公共日志系统 API
 * ============================================================ */
void log_system_init(void) {
    if (initialized) {
        emergency_write_simple("[LogExtra] Already initialized\n");
        return;
    }
    emergency_write_simple("[LogExtra] Initializing log system\n");

    ensure_log_dir();
    update_log_path();
    initialized = 1;

    start_cleanup_thread();

    char msg[256];
    safe_snprintf(msg, sizeof(msg), "[LogExtra] Log system ready, file=%s\n", current_log_path);
    emergency_write_simple(msg);
}

void log_set_level(int level) {
    if (level >= LOG_LEVEL_ERROR && level <= LOG_LEVEL_DEBUG) {
        current_log_level = level;
        g_global_level = level;
        char msg[32];
        safe_snprintf(msg, sizeof(msg), "[LogExtra] Level set to %d\n", level);
        emergency_write_simple(msg);
    }
}

int log_get_level(void) {
    return current_log_level;
}

void log_set_global_level(int level) {
    if (level >= LOG_LEVEL_ERROR && level <= LOG_LEVEL_DEBUG) {
        g_global_level = level;
        current_log_level = level;
        LOG_DEBUG_T("LogExtra", "GlobalLevel", "Set", "global level set to %d", level);
    }
}

int log_get_global_level(void) {
    return g_global_level;
}

int log_get_default_level(void) {
    return LOG_LEVEL_DEBUG;
}

int log_level_from_string(const char *str) {
    if (!str) return -1;
    if (strcmp(str, "debug") == 0) return LOG_LEVEL_DEBUG;
    if (strcmp(str, "info") == 0) return LOG_LEVEL_INFO;
    if (strcmp(str, "warn") == 0) return LOG_LEVEL_WARN;
    if (strcmp(str, "error") == 0) return LOG_LEVEL_ERROR;
    return -1;
}

const char* log_level_to_string(int level) {
    switch (level) {
        case LOG_LEVEL_DEBUG: return "debug";
        case LOG_LEVEL_INFO:  return "info";
        case LOG_LEVEL_WARN:  return "warn";
        case LOG_LEVEL_ERROR: return "error";
        default:              return "unknown";
    }
}

void log_set_module_level(const char *module, int level) {
    if (!module || !*module) return;
    if (level < LOG_LEVEL_ERROR || level > LOG_LEVEL_DEBUG) return;

    pthread_mutex_lock(&module_lock);
    for (int i = 0; i < module_count; i++) {
        if (strcmp(module_levels[i].name, module) == 0) {
            module_levels[i].level = level;
            pthread_mutex_unlock(&module_lock);
            LOG_DEBUG_T("LogExtra", "ModuleLevel", "Set", "module '%s' level set to %d", module, level);
            return;
        }
    }
    if (module_count < MAX_MODULES) {
        safe_strncpy(module_levels[module_count].name, module, sizeof(module_levels[module_count].name));
        module_levels[module_count].level = level;
        module_count++;
        LOG_DEBUG_T("LogExtra", "ModuleLevel", "Set", "module '%s' registered with level %d", module, level);
    }
    pthread_mutex_unlock(&module_lock);
}

int log_get_module_level(const char *module) {
    if (!module || !*module) return -1;
    pthread_mutex_lock(&module_lock);
    for (int i = 0; i < module_count; i++) {
        if (strcmp(module_levels[i].name, module) == 0) {
            int lv = module_levels[i].level;
            pthread_mutex_unlock(&module_lock);
            return lv;
        }
    }
    pthread_mutex_unlock(&module_lock);
    return -1;
}

int log_module_exists(const char *module) {
    if (!module || !*module) return 0;
    pthread_mutex_lock(&module_lock);
    for (int i = 0; i < module_count; i++) {
        if (strcmp(module_levels[i].name, module) == 0) {
            pthread_mutex_unlock(&module_lock);
            return 1;
        }
    }
    pthread_mutex_unlock(&module_lock);
    return 0;
}

int log_reset_module_level(const char *module) {
    if (!module || !*module) return -1;
    pthread_mutex_lock(&module_lock);
    for (int i = 0; i < module_count; i++) {
        if (strcmp(module_levels[i].name, module) == 0) {
            for (int j = i; j < module_count - 1; j++) {
                module_levels[j] = module_levels[j + 1];
            }
            module_count--;
            pthread_mutex_unlock(&module_lock);
            LOG_DEBUG_T("LogExtra", "ModuleLevel", "Reset", "module '%s' reset to default", module);
            return 0;
        }
    }
    pthread_mutex_unlock(&module_lock);
    return -1;
}

int log_reset_all_modules(void) {
    pthread_mutex_lock(&module_lock);
    module_count = 0;
    pthread_mutex_unlock(&module_lock);
    LOG_DEBUG_T("LogExtra", "ModuleLevel", "ResetAll", "all modules reset to default");
    return 0;
}

void log_dump_module_levels(void) {
    pthread_mutex_lock(&module_lock);
    uart_puts(tr("\nModule log levels:\n", "\n模块日志级别：\n"));
    for (int i = 0; i < module_count; i++) {
        const char *lv_str = "???";
        switch (module_levels[i].level) {
            case LOG_LEVEL_ERROR: lv_str = tr("ERROR", "错误"); break;
            case LOG_LEVEL_WARN:  lv_str = tr("WARN", "警告"); break;
            case LOG_LEVEL_INFO:  lv_str = tr("INFO", "信息"); break;
            case LOG_LEVEL_DEBUG: lv_str = tr("DEBUG", "调试"); break;
        }
        char buf[128];
        safe_snprintf(buf, sizeof(buf), "  %s: %s\n", module_levels[i].name, lv_str);
        uart_puts(buf);
    }
    if (module_count == 0) {
        uart_puts(tr("  (no modules with custom levels)\n", "  (没有自定义级别的模块)\n"));
    }
    pthread_mutex_unlock(&module_lock);
}

void log_set_console_output(int enable) {
    console_output = enable;
}

/* 【2026-08-22 定稿】文件保存开关 */
void log_set_file_output(int enable) {
    file_output = enable ? 1 : 0;
}
int log_get_file_output(void) {
    return file_output;
}

/* ============================================================
 * 核心日志输出
 * ============================================================ */
void log_output(int level, const char *module, const char *submodule,
                const char *step, const char *func, const char *fmt, ...) {
    int effective_level = g_global_level;
    if (module) {
        int mod_level = log_get_module_level(module);
        if (mod_level >= 0) {
            effective_level = mod_level;
        }
    }
    if (level > effective_level) return;

    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    struct tm *tm = localtime(&ts.tv_sec);
    char time_buf[32];
    strftime(time_buf, sizeof(time_buf), "%H:%M:%S", tm);
    char ms_buf[8];
    safe_snprintf(ms_buf, sizeof(ms_buf), ".%03ld", ts.tv_nsec / 1000000);
    strcat(time_buf, ms_buf);

    const char *lvl_str = "???";
    const char *color = COLOR_RESET;
    switch (level) {
        case LOG_LEVEL_ERROR: lvl_str = tr("ERROR", "错误"); color = COLOR_RED; break;
        case LOG_LEVEL_WARN:  lvl_str = tr("WARN", "警告");  color = COLOR_YELLOW; break;
        case LOG_LEVEL_INFO:  lvl_str = tr("INFO", "信息");  color = COLOR_DIM; break;
        case LOG_LEVEL_DEBUG: lvl_str = tr("DEBUG", "调试"); color = COLOR_DIM; break;
        default: break;
    }

    char msg_body[MAX_LOG_MSG];
    va_list args;
    va_start(args, fmt);
    vsnprintf(msg_body, sizeof(msg_body), fmt, args);
    va_end(args);

    char full_msg[MAX_LOG_MSG + 256];
    /* 【2026-08-22 定稿】终端时间改 [时间] 括号 */
    int len = safe_snprintf(full_msg, sizeof(full_msg),
                           "%s[%s][%s][%s][%s][%s] %s%s\n",
                           color, time_buf, lvl_str,
                           module ? module : "?",
                           submodule ? submodule : "?",
                           step ? step : "?",
                           func ? func : "?",
                           msg_body, COLOR_RESET);
    if (len < 0 || len >= (int)sizeof(full_msg)) {
        emergency_write_simple("[LogExtra] Log message truncated\n");
        return;
    }

    if (console_output) {
        fprintf(stderr, "%s", full_msg);
        fflush(stderr);
    }

    /* 【2026-08-22 定稿】文件：单文件 JSON 四字段 time/id/level/txt
     * - 开关 file_output=1(默认) → DEBUG 全量写；=0 → 仅 WARN+（level<=WARN）
     * 条件：开=写全部（level 恒<=DEBUG）；关=仅 WARN+ */
    if ((file_output && level <= LOG_LEVEL_DEBUG) ||
        (!file_output && level <= LOG_LEVEL_WARN)) {
        update_log_path();
        if (current_log_path[0] != '\0') {
            FILE *fp = fopen(current_log_path, "a");
            if (fp) {
                long seq = ++log_seq;
                /* 组装 txt：纯终端正文（含 [LEVEL][module][submodule][step][func] 标识） */
                char plain_msg[MAX_LOG_MSG + 256];
                safe_snprintf(plain_msg, sizeof(plain_msg),
                              "[%s][%s][%s][%s][%s] %s",
                              lvl_str,
                              module ? module : "?",
                              submodule ? submodule : "?",
                              step ? step : "?",
                              func ? func : "?",
                              msg_body);
                /* ISO8601 带时区毫秒 */
                char iso_buf[64];
                char tz_buf[8];
                strftime(iso_buf, sizeof(iso_buf), "%Y-%m-%dT%H:%M:%S", tm);
                strftime(tz_buf, sizeof(tz_buf), "%z", tm);
                char *plain_esc = NULL;
                size_t plain_len = strlen(plain_msg);
                /* 简单 JSON 转义（引号/反斜杠/控制字符）——防弹 */
                char *esc = malloc(plain_len * 6 + 1);
                if (esc) {
                    size_t j = 0;
                    for (size_t i = 0; i < plain_len && j < plain_len * 6; i++) {
                        unsigned char c = (unsigned char)plain_msg[i];
                        switch (c) {
                            case '"':  esc[j++] = '\\'; esc[j++] = '"'; break;
                            case '\\': esc[j++] = '\\'; esc[j++] = '\\'; break;
                            case '\n': esc[j++] = '\\'; esc[j++] = 'n'; break;
                            case '\r': esc[j++] = '\\'; esc[j++] = 'r'; break;
                            case '\t': esc[j++] = '\\'; esc[j++] = 't'; break;
                            default:
                                if (c < 0x20) { esc[j++] = '\\'; esc[j++] = 'u'; esc[j++] = '0'; esc[j++] = '0'; esc[j++] = "0123456789abcdef"[c>>4]; esc[j++] = "0123456789abcdef"[c&15]; }
                                else esc[j++] = (char)c;
                        }
                    }
                    esc[j] = '\0';
                    fprintf(fp, "{\"time\":\"%s.%03ld%s\",\"id\":%ld,\"level\":\"%s\",\"txt\":\"%s\"}\n",
                            iso_buf, ts.tv_nsec / 1000000, tz_buf, seq, lvl_str, esc);
                    free(esc);
                } else {
                    /* 内存不足跛脚：降级写无转义 */
                    fprintf(fp, "{\"time\":\"%s.%03ld%s\",\"id\":%ld,\"level\":\"%s\",\"txt\":\"%s\"}\n",
                            iso_buf, ts.tv_nsec / 1000000, tz_buf, seq, lvl_str, plain_msg);
                }
                fclose(fp);
            }
        }
    }
}

void log_dump_all(void) {
    update_log_path();
    FILE *fp = fopen(current_log_path, "r");
    if (!fp) {
        emergency_write_simple(tr("No log file found.\n", "未找到日志文件。\n"));
        return;
    }
    char buf[4096];
    while (fgets(buf, sizeof(buf), fp)) {
        fputs(buf, stdout);
    }
    fclose(fp);
}

/* ============================================================
 * UI 辅助函数
 * ============================================================ */
int log_get_terminal_width(void) {
    struct winsize w;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &w) == 0 && w.ws_col > 0) {
        return w.ws_col;
    }
    return 80;
}

void log_color_puts(const char *color, const char *prefix, const char *fmt, ...) {
    if (!color) color = COLOR_RESET;
    if (!prefix) prefix = "";
    char msg_buf[4096];
    va_list args;
    va_start(args, fmt);
    vsnprintf(msg_buf, sizeof(msg_buf), fmt, args);
    va_end(args);

    size_t len = strlen(msg_buf);
    if (len > 0 && msg_buf[len-1] == '\n') msg_buf[len-1] = '\0';

    uart_puts(color);
    if (prefix[0]) {
        uart_puts("[");
        uart_puts(prefix);
        uart_puts("] ");
    }
    uart_puts(msg_buf);
    uart_puts(COLOR_RESET);
    uart_puts("\n");
}

void log_draw_box(const char *title, const char *content,
                  const char *title_color, const char *border_color, const char *content_color) {
    if (!content) return;
    if (!title_color) title_color = COLOR_CYAN;
    if (!border_color) border_color = COLOR_DIM;
    if (!content_color) content_color = COLOR_RESET;

    int width = log_get_terminal_width();
    if (width > 120) width = 120;
    if (width < 40) width = 40;

    int title_len = title ? strlen(title) : 0;
    int box_width = width - 4;
    if (box_width < 10) box_width = 10;

    uart_puts(border_color);
    uart_puts("┌");
    if (title && title_len > 0) {
        int title_space = box_width - title_len - 2;
        if (title_space < 0) title_space = 0;
        uart_puts(" ");
        uart_puts(title_color);
        uart_puts(title);
        uart_puts(border_color);
        for (int i = 0; i < title_space; i++) uart_puts("─");
    } else {
        for (int i = 0; i < box_width; i++) uart_puts("─");
    }
    uart_puts("┐");
    uart_puts(COLOR_RESET);
    uart_puts("\n");

    char line_buf[1024];
    const char *p = content;
    while (*p) {
        int line_len = 0;
        while (*p && line_len < box_width - 2) {
            if (*p == '\n') {
                p++;
                break;
            }
            line_buf[line_len++] = *p++;
        }
        line_buf[line_len] = '\0';

        uart_puts(border_color);
        uart_puts("│ ");
        uart_puts(content_color);
        uart_puts(line_buf);
        int padding = box_width - 2 - line_len;
        if (padding > 0) {
            for (int i = 0; i < padding; i++) uart_puts(" ");
        }
        uart_puts(border_color);
        uart_puts(" │");
        uart_puts(COLOR_RESET);
        uart_puts("\n");
    }

    uart_puts(border_color);
    uart_puts("└");
    for (int i = 0; i < box_width; i++) uart_puts("─");
    uart_puts("┘");
    uart_puts(COLOR_RESET);
    uart_puts("\n");
}

/* ============================================================
 * FTF[绘制状态栏（已修复：空指针检查 + 行清除 + 换行）]
 * ============================================================ */
void log_draw_status_bar(const char *version, int ai_status, const char *mode, int task_count) {
    /* ---- 空指针检查 ---- */
    if (!version) version = "unknown";
    if (!mode) mode = "unknown";

    int width = log_get_terminal_width();
    if (width > 120) width = 120;
    if (width < 40) width = 40;

    /* 清除当前行，防止与提示符混在一起 */
    uart_puts("\r\033[K");

    const char *status_color = ai_status ? COLOR_GREEN : COLOR_RED;
    const char *status_symbol = ai_status ? "●" : "○";

    const char *ling_os = tr("LING OS", "LING OS");
    const char *mode_text = tr("Mode", "模式");
    const char *tasks_text = tr("Tasks", "任务");

    if (!ling_os) ling_os = "LING OS";
    if (!mode_text) mode_text = "Mode";
    if (!tasks_text) tasks_text = "Tasks";

    char status_line[512];
    safe_snprintf(status_line, sizeof(status_line),
             "%s%s %s%s %s | %s %s | %s %d%s",
             COLOR_BOLD, COLOR_CYAN,
             status_color, status_symbol,
             COLOR_WHITE, ling_os,
             COLOR_DIM, mode_text, mode,
             tasks_text, task_count, COLOR_RESET);

    int len = strlen(status_line);
    if (len < width) {
        int pad = (width - len) / 2;
        if (pad < 0) pad = 0;
        for (int i = 0; i < pad; i++) uart_puts(" ");
    }
    uart_puts(status_line);
    uart_puts("\n");   /* 强制换行，确保与提示符分离 */
    fflush(stdout);
}

/* ============================================================
 * 增强版进度条（支持速度/大小/耗时）
 * ============================================================ */
static const char PROGRESS_CHARS[] = {' ', '▏', '▎', '▍', '▌', '▋', '▊', '▉', '█'};
static const int PROGRESS_LEVELS = 8;
static int g_progress_width = 30;

int log_get_progress_width(void) {
    return g_progress_width;
}

void log_set_progress_width(int width) {
    if (width < 10) width = 10;
    if (width > 60) width = 60;
    g_progress_width = width;
}

/**
 * @brief 增强版进度条（完整信息）
 * @param percent 进度百分比 (0-100)
 * @param speed 速度 (MB/s，0 表示不显示)
 * @param downloaded 已下载 (MB，0 表示不显示)
 * @param total 总大小 (MB，0 表示不显示)
 * @param elapsed 已耗时 (秒，0 表示不显示)
 * @param label 标签
 * @param status 状态 (PROGRESS_IDLE/RUNNING/DONE/FAILED)
 */
void log_draw_progress_full(int percent, double speed, double downloaded,
                            double total, int elapsed, const char *label,
                            progress_status_t status) {
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;

    const char *color = COLOR_RESET;
    switch (status) {
        case PROGRESS_IDLE:    color = COLOR_DIM; break;
        case PROGRESS_RUNNING: color = COLOR_CYAN; break;
        case PROGRESS_DONE:    color = COLOR_GREEN; break;
        case PROGRESS_FAILED:  color = COLOR_RED; break;
        default:               color = COLOR_RESET; break;
    }

    int width = g_progress_width;
    int total_units = width * PROGRESS_LEVELS;
    int filled = (percent * total_units) / 100;
    int full_blocks = filled / PROGRESS_LEVELS;
    int partial = filled % PROGRESS_LEVELS;

    char line[512];
    int pos = 0;

    /* 标签 */
    if (label && label[0]) {
        pos += safe_snprintf(line + pos, sizeof(line) - pos, "%s ", label);
    }

    /* 进度条 */
    pos += safe_snprintf(line + pos, sizeof(line) - pos, "[");
    for (int i = 0; i < full_blocks && i < width; i++) {
        pos += safe_snprintf(line + pos, sizeof(line) - pos, "█");
    }
    if (full_blocks < width) {
        pos += safe_snprintf(line + pos, sizeof(line) - pos, "%c", PROGRESS_CHARS[partial]);
        for (int i = full_blocks + 1; i < width; i++) {
            pos += safe_snprintf(line + pos, sizeof(line) - pos, " ");
        }
    }
    pos += safe_snprintf(line + pos, sizeof(line) - pos, "] ");

    /* 百分比 */
    pos += safe_snprintf(line + pos, sizeof(line) - pos, "%3d%%", percent);

    /* 速度 */
    if (speed > 0) {
        pos += safe_snprintf(line + pos, sizeof(line) - pos, "  速度: %.1fMB/s", speed);
    }

    /* 已下载/总大小 */
    if (total > 0 && downloaded > 0) {
        pos += safe_snprintf(line + pos, sizeof(line) - pos, "  已下载: %.1f/%.1fMB", downloaded, total);
    } else if (downloaded > 0) {
        pos += safe_snprintf(line + pos, sizeof(line) - pos, "  已下载: %.1fMB", downloaded);
    }

    /* 耗时 */
    if (elapsed > 0) {
        pos += safe_snprintf(line + pos, sizeof(line) - pos, "  耗时: %ds", elapsed);
    }

    /* 状态图标 */
    if (status == PROGRESS_DONE) {
        pos += safe_snprintf(line + pos, sizeof(line) - pos, " ✅");
    } else if (status == PROGRESS_FAILED) {
        pos += safe_snprintf(line + pos, sizeof(line) - pos, " ❌");
    } else if (status == PROGRESS_RUNNING) {
        pos += safe_snprintf(line + pos, sizeof(line) - pos, " ⠋");
    }

    uart_puts("\r\033[K");
    uart_puts(line);
    fflush(stdout);
}

/* ============================================================
 * 原有简单进度条（兼容旧 API）
 * ============================================================ */
void log_draw_progress(int percent, const char *label, progress_status_t status) {
    log_draw_progress_full(percent, 0, 0, 0, 0, label, status);
}

int log_update_progress_async(const char *label_prefix, int max_retries) {
    (void)max_retries;
    const char *progress_file = "/tmp/progress.txt";
    FILE *fp = fopen(progress_file, "r");
    if (!fp) {
        if (label_prefix) {
            log_draw_progress(0, label_prefix, PROGRESS_IDLE);
        }
        return 0;
    }

    char line[256];
    if (!fgets(line, sizeof(line), fp)) {
        fclose(fp);
        return 0;
    }
    fclose(fp);

    int percent = 0;
    char label[128] = {0};
    int status_int = 0;
    double speed = 0;
    double downloaded = 0;
    double total = 0;
    int elapsed = 0;

    char *p1 = strtok(line, "|");
    char *p2 = strtok(NULL, "|");
    char *p3 = strtok(NULL, "|");
    char *p4 = strtok(NULL, "|");
    char *p5 = strtok(NULL, "|");
    char *p6 = strtok(NULL, "|");

    if (p1) percent = atoi(p1);
    if (p2) safe_strncpy(label, p2, sizeof(label));
    if (p3) status_int = atoi(p3);
    if (p4) speed = atof(p4);
    if (p5) downloaded = atof(p5);
    if (p6) total = atof(p6);

    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;

    progress_status_t status = (progress_status_t)status_int;

    char full_label[256];
    if (label_prefix && label_prefix[0]) {
        safe_snprintf(full_label, sizeof(full_label), "%s %s", label_prefix, label);
    } else {
        safe_strncpy(full_label, label, sizeof(full_label));
    }

    log_draw_progress_full(percent, speed, downloaded, total, elapsed,
                           full_label[0] ? full_label : NULL, status);

    if (status == PROGRESS_DONE) return 1;
    if (status == PROGRESS_FAILED) return -1;
    return 0;
}

void log_write_progress_file(int percent, const char *label, progress_status_t status) {
    const char *progress_file = "/tmp/progress.txt";
    FILE *fp = fopen(progress_file, "w");
    if (!fp) return;
    fprintf(fp, "%d|%s|%d|0|0|0\n", percent, label ? label : "", (int)status);
    fclose(fp);
}

void log_write_progress_file_full(int percent, const char *label, progress_status_t status,
                                  double speed, double downloaded, double total) {
    const char *progress_file = "/tmp/progress.txt";
    FILE *fp = fopen(progress_file, "w");
    if (!fp) return;
    fprintf(fp, "%d|%s|%d|%.2f|%.2f|%.2f\n",
            percent, label ? label : "", (int)status, speed, downloaded, total);
    fclose(fp);
}

int log_run_with_progress(const char *cmd, const char *label, int timeout_sec) {
    pid_t pid = fork();
    if (pid == 0) {
        setsid();
        execl("/bin/sh", "sh", "-c", cmd, (char *)NULL);
        _exit(1);
    } else if (pid > 0) {
        int elapsed = 0;
        char progress_label[128];
        safe_snprintf(progress_label, sizeof(progress_label), "%s", label ? label : tr("Executing", "执行中"));

        while (elapsed < timeout_sec) {
            int ret = log_update_progress_async(progress_label, 1);
            if (ret == 1) {
                log_draw_progress(100, progress_label, PROGRESS_DONE);
                uart_puts("\n");
                return 0;
            } else if (ret == -1) {
                log_draw_progress(100, progress_label, PROGRESS_FAILED);
                uart_puts("\n");
                return -1;
            }

            int status;
            pid_t result = waitpid(pid, &status, WNOHANG);
            if (result == pid) {
                if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
                    log_draw_progress(100, progress_label, PROGRESS_DONE);
                    uart_puts("\n");
                    return 0;
                } else {
                    log_draw_progress(100, progress_label, PROGRESS_FAILED);
                    uart_puts("\n");
                    return -1;
                }
            }

            sleep(1);
            elapsed++;
        }

        kill(pid, SIGTERM);
        sleep(1);
        kill(pid, SIGKILL);
        log_draw_progress(0, tr("Timeout", "超时"), PROGRESS_FAILED);
        uart_puts("\n");
        return -2;
    } else {
        return -1;
    }
}