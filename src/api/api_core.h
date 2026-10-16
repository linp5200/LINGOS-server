#ifndef API_API_CORE_H
#define API_API_CORE_H
#include "../common/types.h"
#include <stdint.h>   /* 【第 4 期】Linux 下 common/types.h 为空（__linux__ 跳过），
                       * uint32_t 需显式引入——否则任何先包含本头的 TU 都编译失败 */

typedef int (*api_handler_t)(const char *req, char *resp, uint32_t len);
void api_core_init(int force_network);
int api_register(const char *endpoint, api_handler_t handler);
int api_call(const char *endpoint, const char *req, char *resp, uint32_t len);
int api_handle_status(const char *req, char *resp, uint32_t len);
int api_handle_perm_list(const char *req, char *resp, uint32_t len);
int api_handle_fs_info(const char *req, char *resp, uint32_t len);
#endif