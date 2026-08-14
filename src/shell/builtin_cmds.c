/**
 * @file    builtin_cmds.c
 * @brief   内置命令实现（ls, cp, mv, pwd）
 * @version 2.0.0.0
 *
 * 这些命令在 LING OS 内部实现，不依赖宿主系统。
 * 路径解析基于 /LINGOS 根目录。
 */

#include "builtin_cmds.h"
#include "../lib/platform.h"
#include "../common/lang.h"
#include "log_extra.h"
#include "uart.h"
#include "../common/data_path.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <unistd.h>
#include <sys/stat.h>
#include <fcntl.h>

/**
 * @brief 将用户路径转换为绝对路径（基于 /LINGOS）
 * @param input 用户输入的路径（可相对或绝对）
 * @param output 输出缓冲区
 * @param out_size 缓冲区大小
 */
static void resolve_path(const char *input, char *output, size_t out_size) {
    const char *root = lingos_data_root();
    if (input[0] == '/') {
        snprintf(output, out_size, "%s%s", root, input);
    } else {
        char cwd[1024];
        if (getcwd(cwd, sizeof(cwd)) == NULL) {
            snprintf(cwd, sizeof(cwd), "%s", root);
        }
        snprintf(output, out_size, "%s/%s", cwd, input);
    }
}

/**
 * @brief 列出目录内容
 */
void cmd_ls(const char *arg) {
    char path[1024];
    if (arg && arg[0]) {
        resolve_path(arg, path, sizeof(path));
    } else {
        const char *root = lingos_data_root();
        snprintf(path, sizeof(path), "%s", root);
    }
    DIR *d = opendir(path);
    if (!d) {
        uart_puts(tr("ls: cannot open ", "ls: 无法打开 "));
        uart_puts(arg ? arg : ".");
        uart_puts("\n");
        return;
    }
    struct dirent *entry;
    while ((entry = readdir(d)) != NULL) {
        if (entry->d_name[0] == '.' && !arg) continue;
        uart_puts(entry->d_name);
        uart_puts("\n");
    }
    closedir(d);
}

/**
 * @brief 复制文件
 */
void cmd_cp(const char *src, const char *dst) {
    char src_path[1024], dst_path[1024];
    resolve_path(src, src_path, sizeof(src_path));
    resolve_path(dst, dst_path, sizeof(dst_path));
    int src_fd = open(src_path, O_RDONLY);
    if (src_fd < 0) {
        uart_puts(tr("cp: cannot open ", "cp: 无法打开 "));
        uart_puts(src);
        uart_puts("\n");
        return;
    }
    int dst_fd = open(dst_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (dst_fd < 0) {
        close(src_fd);
        uart_puts(tr("cp: cannot create ", "cp: 无法创建 "));
        uart_puts(dst);
        uart_puts("\n");
        return;
    }
    char buf[8192];
    ssize_t n;
    while ((n = read(src_fd, buf, sizeof(buf))) > 0) {
        write(dst_fd, buf, n);
    }
    close(src_fd);
    close(dst_fd);
    uart_puts(tr("cp: done\n", "cp: 完成\n"));
}

/**
 * @brief 移动/重命名文件
 */
void cmd_mv(const char *src, const char *dst) {
    char src_path[1024], dst_path[1024];
    resolve_path(src, src_path, sizeof(src_path));
    resolve_path(dst, dst_path, sizeof(dst_path));
    if (rename(src_path, dst_path) == 0) {
        uart_puts(tr("mv: done\n", "mv: 完成\n"));
    } else {
        uart_puts(tr("mv: failed\n", "mv: 失败\n"));
    }
}

/**
 * @brief 显示当前工作目录（相对于 /LINGOS）
 */
void cmd_pwd(void) {
    char cwd[1024];
    const char *root = lingos_data_root();
    if (getcwd(cwd, sizeof(cwd)) == NULL) {
        uart_puts(root);
    } else {
        if (strncmp(cwd, root, strlen(root)) == 0) {
            if (strlen(cwd) == strlen(root)) {
                uart_puts("/");
            } else {
                uart_puts(cwd + strlen(root));
            }
        } else {
            uart_puts(cwd);
        }
    }
    uart_puts("\n");
}