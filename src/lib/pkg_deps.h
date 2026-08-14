#ifndef LIB_PKG_DEPS_H
#define LIB_PKG_DEPS_H

/* 解析依赖（递归），返回依赖包列表（空格分隔字符串，需 free）*/
char *pkg_resolve_deps(const char *package_path);

/* 下载依赖包（调用 apt-get download）*/
int pkg_download_deps(const char *deps_list);

#endif