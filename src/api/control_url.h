/**
 * @file    src/api/control_url.h
 * @brief   Control URL —— 统一控制入口（意图 → 标准化执行面）
 * @version LN-0.8.0
 *
 * 【第 4 期】先生创意出处（IntentCP 借鉴）：
 *   docs/LINGOS-完整功能核对与优化方案-v2-20260912.md:199
 *   docs/LINGOS-SDK开发方案-20260925.md:324
 *   docs/LINGOS-全部工作总账-20260913.md:140
 *     → \`lingos://control/scene.apply?id=movie\`：意图 → 执行标准化
 *   原意是"NL/意图 → 标准执行面"的**统一寻址格式**，而非某个具体业务功能。
 *
 * 本实现（判定依据见交付报告）：
 *   · 规范形式 lingos://control/<系统.动作>?k=v&k2=v2
 *   · HTTP 入口  GET|POST /api/control （src/api/http_server.c 接线）
 *   · 本地入口  shell 命令 control <url|动作>（src/shell/shell.c 接线）
 *   · 内部入口  api_call("control", ...)（src/api/api_core.c 注册）
 *   三条入口共用本文件同一实现，动作表里点分规范名与旧别名并存（旧命令一律保留）。
 *   未实现的动作（如 docs 里的 scene.apply 在服务端 C 代码中无场景子系统）
 *   **诚实报错**，绝不返回假成功。
 */
#ifndef API_CONTROL_URL_H
#define API_CONTROL_URL_H

#include <stddef.h>

/* 解析 Control URL / 裸动作：
 *   "lingos://control/system.status?x=1" → action="system.status", query="x=1"
 *   "system.status?x=1"                  → 同上（CLI 简写）
 * 返回 0 成功；-1 非法（含动作字符集校验失败）。 */
int control_url_parse(const char *url, char *action, size_t action_size,
                      char *query, size_t query_size);

/* 执行 Control URL：解析 → 动作映射 → api_call → 标准信封 JSON（m6.txt §5.2）
 *   {"id":..,"code":0,"message":"success","action":..,"endpoint":..,"data":{..}}
 * 未知/未实现动作 → 诚实错误信封（code 404/501）；缓冲区不足 → -1。 */
int control_url_execute(const char *url, const char *id, char *resp, size_t resp_size);

/* 直接按动作名 + JSON 参数执行（HTTP 简写 {"command":..,"args":{..}} 用）；
 * args_json 为 NULL 时按 "{}" 处理。返回语义同 control_url_execute。 */
int control_url_execute_action(const char *action, const char *args_json,
                               const char *id, char *resp, size_t resp_size);

/* 输出可用动作 JSON 数组（发现性；错误提示用），返回动作数，-1 缓冲区不足 */
int control_url_list_actions(char *out, size_t out_size);

#endif /* API_CONTROL_URL_H */
