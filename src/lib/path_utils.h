#ifndef LIB_PATH_UTILS_H
#define LIB_PATH_UTILS_H

/* 将路径中的前导 ~ 展开为家目录。
 * 返回静态缓冲区指针（每次调用覆盖），失败则返回原字符串。
 */
const char *expand_tilde(const char *path);

#endif