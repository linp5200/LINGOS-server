/**
 * @file    distro_detect.h
 * @brief   发行版自动检测头文件
 * @version LN-B-4.3.0.0
 */

#ifndef COMMON_DISTRO_DETECT_H
#define COMMON_DISTRO_DETECT_H

typedef enum {
    DISTRO_UNKNOWN = 0,
    DISTRO_DEBIAN,
    DISTRO_FEDORA,
    DISTRO_ARCH,
    DISTRO_OPENSUSE,
    DISTRO_ALPINE
} distro_type_t;

#define PKG_MANAGER_UNKNOWN 0
#define PKG_MANAGER_APT     1
#define PKG_MANAGER_DNF     2
#define PKG_MANAGER_YUM     3
#define PKG_MANAGER_PACMAN  4
#define PKG_MANAGER_ZYPPER  5
#define PKG_MANAGER_APK     6

typedef struct {
    distro_type_t distro_type;
    char id[64];
    char name[128];
    char version[64];
} distro_info_t;

distro_info_t distro_detect(void);
int distro_get_package_manager(distro_type_t type);
const char* distro_package_manager_name(int pm);
int distro_package_manager_available(int pm);
const char* distro_type_name(distro_type_t type);

#endif /* COMMON_DISTRO_DETECT_H */