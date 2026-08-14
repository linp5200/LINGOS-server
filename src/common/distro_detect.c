/**
 * @file    src/common/distro_detect.c
 * @brief   发行版自动检测
 * @version LN-B-4.3.0.0
 * @changes 修复数组左值错误；移除不存在的 id_like 成员引用
 * @par     核心协议：防御性编程
 */

#include "distro_detect.h"
#include "safe_string.h"
#include "../lib/log_extra.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <unistd.h>

/* ============================================================
 * 获取发行版信息
 * ============================================================ */

distro_info_t distro_detect(void) {
    LOG_DEBUG_T("DistroDetect", "Detect", "Enter", "detecting distribution");

    distro_info_t info;
    memset(&info, 0, sizeof(info));
    info.distro_type = DISTRO_UNKNOWN;

    /* 尝试读取 /etc/os-release */
    FILE *fp = fopen("/etc/os-release", "r");
    if (fp) {
        char line[256];
        char id[64] = {0};
        char id_like[128] = {0};
        char name[128] = {0};
        char version[64] = {0};
        char *p, *q;

        while (fgets(line, sizeof(line), fp)) {
            /* 去除尾部换行 */
            char *nl = strchr(line, '\n');
            if (nl) *nl = '\0';

            if (strncmp(line, "ID=", 3) == 0) {
                p = line + 3;
                if (*p == '"') p++;
                q = strchr(p, '"');
                if (q) *q = '\0';
                safe_strncpy(id, p, sizeof(id));
            } else if (strncmp(line, "ID_LIKE=", 8) == 0) {
                p = line + 8;
                if (*p == '"') p++;
                q = strchr(p, '"');
                if (q) *q = '\0';
                safe_strncpy(id_like, p, sizeof(id_like));
            } else if (strncmp(line, "NAME=", 5) == 0) {
                p = line + 5;
                if (*p == '"') p++;
                q = strchr(p, '"');
                if (q) *q = '\0';
                safe_strncpy(name, p, sizeof(name));
            } else if (strncmp(line, "VERSION_ID=", 11) == 0) {
                p = line + 11;
                if (*p == '"') p++;
                q = strchr(p, '"');
                if (q) *q = '\0';
                safe_strncpy(version, p, sizeof(version));
            }
        }
        fclose(fp);

        /* 复制到 info 结构体 */
        safe_strncpy(info.id, id, sizeof(info.id));
        safe_strncpy(info.name, name, sizeof(info.name));
        safe_strncpy(info.version, version, sizeof(info.version));

        /* 确定发行版类型 */
        if (strstr(id, "ubuntu") || strstr(id, "debian") ||
            strstr(id_like, "debian") || strstr(id_like, "ubuntu")) {
            info.distro_type = DISTRO_DEBIAN;
        } else if (strstr(id, "fedora") || strstr(id, "rhel") ||
                   strstr(id, "centos") || strstr(id_like, "fedora") ||
                   strstr(id_like, "rhel")) {
            info.distro_type = DISTRO_FEDORA;
        } else if (strstr(id, "arch") || strstr(id_like, "arch")) {
            info.distro_type = DISTRO_ARCH;
        } else if (strstr(id, "opensuse") || strstr(id, "suse") ||
                   strstr(id_like, "suse")) {
            info.distro_type = DISTRO_OPENSUSE;
        } else if (strstr(id, "alpine") || strstr(id_like, "alpine")) {
            info.distro_type = DISTRO_ALPINE;
        } else {
            info.distro_type = DISTRO_UNKNOWN;
        }

        LOG_INFO_T("DistroDetect", "Detect", "OK", "detected: %s (%s) type=%d",
                   info.id, info.name, info.distro_type);
        return info;
    }

    /* 备选：检查 /etc/issue */
    fp = fopen("/etc/issue", "r");
    if (fp) {
        char line[256];
        if (fgets(line, sizeof(line), fp)) {
            if (strstr(line, "Ubuntu") || strstr(line, "Debian")) {
                info.distro_type = DISTRO_DEBIAN;
                safe_strncpy(info.id, "debian", sizeof(info.id));
                safe_strncpy(info.name, "Debian/Ubuntu", sizeof(info.name));
            } else if (strstr(line, "Fedora") || strstr(line, "Red Hat") || strstr(line, "CentOS")) {
                info.distro_type = DISTRO_FEDORA;
                safe_strncpy(info.id, "fedora", sizeof(info.id));
                safe_strncpy(info.name, "Fedora/RHEL", sizeof(info.name));
            } else if (strstr(line, "Arch")) {
                info.distro_type = DISTRO_ARCH;
                safe_strncpy(info.id, "arch", sizeof(info.id));
                safe_strncpy(info.name, "Arch Linux", sizeof(info.name));
            } else if (strstr(line, "openSUSE") || strstr(line, "SUSE")) {
                info.distro_type = DISTRO_OPENSUSE;
                safe_strncpy(info.id, "opensuse", sizeof(info.id));
                safe_strncpy(info.name, "openSUSE", sizeof(info.name));
            } else if (strstr(line, "Alpine")) {
                info.distro_type = DISTRO_ALPINE;
                safe_strncpy(info.id, "alpine", sizeof(info.id));
                safe_strncpy(info.name, "Alpine Linux", sizeof(info.name));
            }
        }
        fclose(fp);
        LOG_INFO_T("DistroDetect", "Detect", "OK", "detected via /etc/issue: type=%d", info.distro_type);
        return info;
    }

    LOG_WARN_T("DistroDetect", "Detect", "Unknown", "could not detect distribution");
    return info;
}

/* ============================================================
 * 获取包管理器类型
 * ============================================================ */

int distro_get_package_manager(distro_type_t type) {
    if (type == DISTRO_DEBIAN)   return PKG_MANAGER_APT;
    if (type == DISTRO_FEDORA)   return PKG_MANAGER_DNF;
    if (type == DISTRO_ARCH)     return PKG_MANAGER_PACMAN;
    if (type == DISTRO_OPENSUSE) return PKG_MANAGER_ZYPPER;
    if (type == DISTRO_ALPINE)   return PKG_MANAGER_APK;
    return PKG_MANAGER_UNKNOWN;
}

/* ============================================================
 * 获取包管理器名称字符串
 * ============================================================ */

const char* distro_package_manager_name(int pm) {
    switch (pm) {
        case PKG_MANAGER_APT:    return "apt";
        case PKG_MANAGER_DNF:    return "dnf";
        case PKG_MANAGER_YUM:    return "yum";
        case PKG_MANAGER_PACMAN: return "pacman";
        case PKG_MANAGER_ZYPPER: return "zypper";
        case PKG_MANAGER_APK:    return "apk";
        default:                 return "unknown";
    }
}

/* ============================================================
 * 检测指定的包管理器是否可用
 * ============================================================ */

int distro_package_manager_available(int pm) {
    const char *cmd = NULL;
    switch (pm) {
        case PKG_MANAGER_APT:    cmd = "command -v apt > /dev/null 2>&1"; break;
        case PKG_MANAGER_DNF:    cmd = "command -v dnf > /dev/null 2>&1"; break;
        case PKG_MANAGER_YUM:    cmd = "command -v yum > /dev/null 2>&1"; break;
        case PKG_MANAGER_PACMAN: cmd = "command -v pacman > /dev/null 2>&1"; break;
        case PKG_MANAGER_ZYPPER: cmd = "command -v zypper > /dev/null 2>&1"; break;
        case PKG_MANAGER_APK:    cmd = "command -v apk > /dev/null 2>&1"; break;
        default: return 0;
    }
    return (system(cmd) == 0);
}

/* ============================================================
 * 获取发行版名称字符串
 * ============================================================ */

const char* distro_type_name(distro_type_t type) {
    switch (type) {
        case DISTRO_DEBIAN:   return "Debian/Ubuntu";
        case DISTRO_FEDORA:   return "Fedora/RHEL";
        case DISTRO_ARCH:     return "Arch Linux";
        case DISTRO_OPENSUSE: return "openSUSE";
        case DISTRO_ALPINE:   return "Alpine Linux";
        default:              return "Unknown";
    }
}