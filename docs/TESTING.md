# LING OS 测试指南（本地可跑 + CI + 真机）

> 目标：每个改动都能被至少一层测试拦住。
> 根目录：repos/LINGOS-server（Python/C）、repos/LINGOS-APP（Flutter）。

## 一、本地可跑（无需真机、无需模型 API）

### 1) 契约测试（快，~5 秒）
    python3 tests/test_chat_contract.py

覆盖：_send_evt 注入 ts/seq、round_start/round_end 事件、预警级别归一化（L0=紧急、0=info）、
session_history 参数兼容、命令回包 trace_id/duration_ms/server_ts。

### 2) 本地全量套件（~20 秒）
    python3 tests/test_local_suite.py

覆盖 50 项断言（含用户管理 13 项）：
- A 命令面路由：ext_dispatch 的 ingest_*/remote_status（模拟 /api/cmd 路径）
- B ingest 管线：文本、本地 HTTP 网页、RSS、插件发现（内置+自定义）、错误诚实性（不存在文件必须报 error）
- C MCP 客户端：本地 mock MCP（tests/mock_mcp_server.py）initialize / tools/list / tools/call
- D 服务端函数：weather_code_map / alert_query.available / session_history / weather_current 诚实回退
- E WebUI 静态自检：id 接线、导航↔页面一致、命令面板/投放箱/主题层存在
- F remote_access 状态结构（HTTP/WS/VNC）
- G 用户管理：默认 owner / PBKDF2 密码 / 角色 / 最后 owner 保护 / 重复与弱密码拒绝

### 3) Python 语法/导入
    cd src/python && python3 -m compileall -q .

### 4) WebUI JS 语法
    python3 - <<'PY'
    import re
    src=open('webui/index.html',encoding='utf-8').read()
    scripts=re.findall(r'<script(?:[^>]*)>(.*?)</script>',src,re.S)
    open('/tmp/w.js','w').write('\n;\n'.join(scripts))
    PY
    node --check /tmp/w.js

### 5) 桌面脚本/配置
    bash -n distro/desktop/*.sh
    python3 -c "import xml.etree.ElementTree as ET; ET.parse('distro/desktop/labwc/rc.xml')"
    python3 -c "import json; json.load(open('distro/desktop/waybar/config.jsonc'))"

### 6) App 静态检查（无 Flutter SDK 时的替代）
    python3 /root/MIGRATION-20261007/data/shared/lingos-0.5.0/code/dart_check.py <改动文件...>
    # 注意：括号检查对个别历史文件有已知误报，以 CI 的 flutter analyze 为准

## 二、CI（真正的编译器）

- server：push 后手动触发（Data API 推送不触发 Actions）
    POST /repos/linp5200/LINGOS-server/actions/workflows/build.yml/dispatches {"ref":"main"}
  覆盖：C 全量编译 + bundle（aarch64/x86_64）+ 产物。
- app：同上，workflows/build.yml；覆盖 flutter analyze + Android/Linux 构建。
- 本轮结果：server build-bundle success（93d4bc0d），app analyze+build success（90dfae73）。

## 三、本地不能跑、需要真机/外部条件

| 项 | 原因 | 替代 |
|---|---|---|
| C 单元测试（tests/test_*.c） | 需完整编译依赖；按项目纪律本地不编译 | CI 编译覆盖；真机跑 |
| Flutter 运行时/widget test | 本机无 Flutter SDK | app CI analyze/build |
| Wayland labwc/kiosk/VNC | 无显示设备 | 真机 |
| HA/MQTT 联调 | 无真实设备/实例 | 真机 |
| 语音 STT | 需 whisper 模型 | 真机/配置后 |
| NCNN 真推理 | 需 C++ 构建链 | 真机/后续 |

## 四、断言清单（回归基线）

- 聊天：round_start/round_end 存在；事件带 ts/seq；App 侧 ChatMsg 有 ts/round。
- 预警：L0 保留；0/info 语义；alert_query 有 available/source_health。
- 天气：预报嵌在 data.hourly/daily；城市 geocoding 失败返回 error/geo_ok=false。
- ingest：6 内置来源 + 插件目录发现；失败必须 error；检索返回 hits（可含 semantic）。
- 命令面：ingest_*/remote_* 在 EXT_MAP；回包含 trace_id/duration_ms/server_ts。
- WebUI：无缺失 id、导航与页面一致、主题层/命令面板/投放箱存在。
