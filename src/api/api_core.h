#ifndef API_API_CORE_H
#define API_API_CORE_H
#include "../common/types.h"

typedef int (*api_handler_t)(const char *req, char *resp, uint32_t len);
void api_core_init(int force_network);
int api_register(const char *endpoint, api_handler_t handler);
int api_call(const char *endpoint, const char *req, char *resp, uint32_t len);
int api_handle_status(const char *req, char *resp, uint32_t len);
int api_handle_perm_list(const char *req, char *resp, uint32_t len);
int api_handle_fs_info(const char *req, char *resp, uint32_t len);
#endif