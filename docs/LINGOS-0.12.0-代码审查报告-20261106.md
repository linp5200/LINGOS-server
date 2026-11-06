# LING OS 0.12.0 代码审查报告（2026-11-06）

> 范围：发行版层（distro/、scripts/、.github/、Makefile、webui）、服务端 C 层（src/，258 .c + 250 .h，约 8.2 万行）、Python Agent 层（src/python，41 .py，约 2.45 万行）。
> 方法：只读静态审查 + 交叉 grep；Makefile 目标展开；5 个核心 C 文件 `gcc -fsyntax-only`；`bash -n`/`py_compile`；现有单测（agent_runtime 7、local_suite 50、chat_contract 全通过）；未修改任何业务文件，未构建 ISO，未跑 Vectras。
> 证据等级：标注【已复核】的条目由 Lead 直接读代码确认；其余为静态审查结论，未做攻击验证。标【需实测】的条目需构建/真机确认。

## 0. 结论

1. **0.12.0 不应作为联网设备镜像发布**：C 层存在未认证远程 RCE（WS 栈溢出、token 子串绕过 + 文件上传覆盖 /LINGOS/bin），Python 层存在未鉴权 HTTP 触发危机并放开全部权限。修复前应断网/仅本地使用。
2. **版本与实际能力脱节**：VERSION=0.12.0，但 0.11 安全核心、0.12 本地模型/插件运行时基本未落地；当前更接近“0.10.3 桌面包 + 0.12 骨架”。
3. **源码治理缺失**：本地 HEAD fac7d00（2026-10-18，VERSION 0.9.0），0.10–0.12 大量改动未提交；远端 tag v0.12.0=dc778e72、main=6ea5bba，三方不一致，发布不可复现。
4. **CI 是假绿**：ISO 冒烟在 stage5 就结束，`|| true` 吞掉失败，正好放过 0.12.0 的 rsync rc24 bug。
5. **功能“有脚本”不等于“生效”**：隐私开关、AI 工具闸门、策略矩阵、插件签名、每应用沙箱、VNC/X11 回退、自动更新等均有声明但无执行点或执行错误。

## 1. 版本与源码状态

- VERSION 工作区 0.12.0；本地 git HEAD `fac7d00`（2026-10-18），VERSION 文件在 HEAD 中为 0.9.0；大量 distro/docs/tools 文件未跟踪。
- 远端：tag `v0.12.0` → `dc778e72`；main `6ea5bba`（发布后仅 finish-install）；本地 origin/main 引用陈旧（2ece3f7）。
- 0.12.0 Release：仅 arm64，ISO 3,540,396,032 B，part0/part1 + merge + SHA256；无 x86_64。
- 结论：0.12.0 源码无法从本地历史复现；tag/main/工作区三方不一致，属 P0 工程问题。

## 2. 系统分层与功能

- **发行版层**：build-iso（Ubuntu noble live-build；arm64 用 chroot squashfs + grub-mkrescue）、10 阶段安装器、登录前 GTK/TUI OOBE、Cinnamon 默认 + XFCE 救援 + LightDM、L0–L4 策略脚本、审计/GeoIP/AI 工具闸门/插件/沙箱/功能包、恢复菜单、CI 发布。
- **C 服务端层**：lingosd（daemon.sock、syscall/exec）、HTTP 8080/WS 2939（自研 mhd_compat）、alertd/visiond/monitord/voiced、update/install/backup、registry/ai、shell、health。
- **Python Agent 层**：ai_server（Unix socket、111 个 if 命令 + 98 条 ext）、skill_handlers（72 技能）、permission_gateway、llm_unified、记忆/会话/子 AI/MCP/HA/视觉/语音/危机。
- **数据流**：App/Web → 8080/2939 → ai.sock（Python）→ daemon.sock（C）→ syscall/exec；alertd/visiond → 8088 → Python；monitord → frame_channel → ai_visiond。

## 3. P0（阻断/可直接利用）

| # | 问题 | 位置 | 影响 | 证据 |
|---|---|---|---|---|
| P0-1 | WS 握手栈溢出 | src/api/websocket_server.c:157-164（key 来自 :977-989；监听 :1106-1120 绑 INADDR_ANY；access_control.c:26-27 默认 lan_no_token=1） | 局域网未认证远程 RCE | 【已复核】 |
| P0-2 | token 子串匹配 + 文件上传覆盖 /LINGOS/bin | src/daemon/connection_handler.c:228-257；src/api/http_server.c:139-155、746-752 | 认证绕过 → root RCE | 【已复核】 |
| P0-3 | /api/alert_event 未鉴权触发危机，危机后全权 | src/python/ai_server.py:5943-5970、6056；crisis.py:452-466、276-334；permission_gateway.py:491-494、ai_server.py:2196-2202 | 物理动作（HA 开锁/关燃气）+ AI 免检 | 【已复核】 |
| P0-4 | OOBE 完成不重启 LightDM | distro/images/live/lingos-oobe-apply:21-27；lingos-oobe-session-x11:3 | 首启自动登录循环/进不了 Cinnamon | 【已复核】 |
| P0-5 | 安装器 stage7 把 rsync rc24 当致命错误 | distro/images/live/lingos-install.sh:364 | 复制到 99% 后中止，GRUB/用户/OOBE 未写 | 【已复核】 |
| P0-6 | live ISO 被 LightDM 接管 | distro/images/build-iso.sh（包清单 145-146；hook 306-319 未禁用） | 启动停在登录界面而非安装 | 【已复核】 |
| P0-7 | 加密安装很可能无法启动 | lingos-install.sh:282（argon2id）、262-268（/boot 在 LUKS 内）、无 GRUB_ENABLE_CRYPTODISK | 加密安装后无法引导 | 【静态审查/需实测】 |
| P0-8 | 源码未入库/三方不一致 | git status；tag dc778e72；本地 HEAD fac7d00 | 发布不可复现、无法回滚 | 【已复核】 |

补充 P0 功能阻断：手动/Live/诊断模式无 TTY（lingos-installer.service:14 StandardInput=null + whiptail）——自动安装可用，其余菜单不可交互。

## 4. P1（严重）

### C 层
1. safe_exec 的 NEED_CONFIRM 只置字段不阻断（safe_exec.c:385-397；syscall_handler.c:981-1010）。
2. 8080/2939 绑 INADDR_ANY + 默认 LAN 免 token（access_control.c:26-33；mhd_compat.c:512-525；websocket_server.c:1106-1120）。
3. send_json_response 用 MHD_RESPMEM_PERSISTENT 引用栈缓冲（http_server.c:121-131；栈缓冲 :279-296/:568-577/:586-591；mhd_compat.c:462-477）。
4. WS 断连后 2s 释放 client，chat 线程仍用裸指针（websocket_server.c:1046-1053、438-456）→ UAF。
5. 生命线告警 TLS 降级/明文（alert_sources.c:127-131、347/395/534 verify=0、NMC http）。
6. update_signature_gate 零调用；无 .sig 仅 WARN（update_verify.c:153；system_update.c:499-513）。
7. backup 忽略 cp 返回值、恒返回 0；restore 先 rm -rf 再 cp；备份失败仍升级（core/backup.c:128-162；system_update.c:554-570）。
8. visiond 与 monitord 同时抢 /dev/video0（core/main.c:1521-1529；visiond.c:236；monitord.c:110-113）。
9. safe_exec 无 uid/gid 降权，白名单含 systemctl/apt/pip/python3/awk/sed/tar/curl（safe_exec.c:307-325、45-68）。
10. C 文件 syscall 不调 permission_check_file；file_read 无路径限制，file_write 仅前缀黑名单（syscall_handler.c:476-537、431-448）。
11. lingosd 单线程 accept + 同步 read_line 无超时（lingosd.c:930-960、340-372、294-321）。
12. tokens.json fopen w 无 chmod；/LINGOS/state 0755（connection_handler.c:151-155；fs_layout.c:43）。
13. 病毒扫描未接线；固定 /tmp/virus_scan_<pid>，mkdir 无检查，tar root 解压，打开失败按 CLEAN（security_wire.c:398；virus_scanner.c:149-171）。

### Python 层
1. crisis_trigger/resolve 无鉴权、无自动过期（ai_server.py:5254-5261、4559-4567；crisis.py:366-368）。
2. MCP 工具无权限/审批（ai_server.py:2075-2076、2236-2239；mcp_client.py:216）。
3. 插件高 risk 不弹审批（skill_handlers.py:1994-2006；permission_gateway.py:451-465）。
4. agent_orchestrator 调 call_deepseek_nonstream 读错字段导致 agent_delegate 恒失败，且 :283 绕过权限（agent_orchestrator.py:248-250、283）。
5. perm_set 自授权限（skill_handlers.py:1231-1277；permission_gateway.py:88、451-465）。
6. set_auto_allow 无鉴权关闭高危审批（ai_server.py:5759-5762、2091）。
7. ai_config 双路径（写 /LINGOS/config，读 /LINGOS/system/config；ai_server.py:4782-4800、4927、254）。
8. lingos-ai-tool-gate 等级绕过（:59/:76/:83；未校验 --level；L2 单 device 放行）；且按 <tool_id>.json 拼路径，实际 weather-get.json 的 id 是 tool.weather.get → 100% “工具不存在”。
9. 非流式审批断链（ai_server.py:2777 不传 conn；715-726 仅在 conn 时推；auth 服务 stdout DEVNULL）。
10. 技能禁用不生效（ai_server.py:4422-4436 写；load_skill_schemas/execute_skill 不过滤）。

### 发行版层
1. 隐私 flags 写 bool、读 "off" 字符串 → 摄像头/麦克风/USB/定位关不掉（security-settings:82；privacy-policy:8-21；security-center:134）。
2. L2+ 出站只放行 53/443，DoT 853 被墙；另缺 DHCP 68/NTP（privacy-policy:74-86）。
3. 安全策略文件 0600，普通用户的安全中心/AI 读不到，回退 L1（install.sh:464；oobe-apply:20；security-center；lingos-ai:16-21）。
4. build-iso 用 cp 保留 0600：applications/desktop/xfce/features/skel Desktop 普通用户不可读或不可双击。
5. X11/Wayland 会话及脚本未安装；GRUB 的 X11 回退空操作（build-iso.sh:190/195/249/255-257；install.sh:403-406）。
6. build.yml VERSION 硬编码 0.9.0；build.yml 与 images.yml 双写 Release；publish 合并说明文件名不一致；publish.yml 无空 ISO 守卫。
7. security-settings 的 loginctl $3 取 USER 与 tty 比较，永不匹配 → 远程/seat 校验 fail-open；改等级不触发 privacy-apply。
8. 沙箱 /lib64 硬编码（arm64 无）且无调用方；示例插件装到 /usr/share 但管理器只扫 /opt、/etc；enable 无加载器。
9. OOBE 明文密码写 /tmp（0644）且不删除；GRUB_TIMEOUT=1 使救援菜单不可选。
10. 策略文件 privacy-levels.json/trust-policy.json 全仓无消费方；实际只有 security-policy.json 的 level+flags。

## 5. P2/P3 摘要

- **C 层 P2**：image_buf 用 sizeof(指针) 导致多模态图片全丢；files/list JSON 首项多逗号；文件端点无 realpath/O_NOFOLLOW；/nook/ask 与 /system/health 未认证且昂贵；含 api_key 的修复脚本写 /tmp 0755；repo update mkstemp 恒 EINVAL；update_auto_check 路径 /LINGOS/LINGOS；registry 跨进程无锁；alert_history system rm -rf；http_client 重定向绕过 egress；api_routes 返回伪造空预警/无更新；旧 permission API 恒允许；部分写；WS subscribe 未认证；MHD 每连接线程/64MB 无上限；MQTT 无 TLS；args_json 泄漏。
- **Python P2**：文件 mkdir/copy/move 绕 _file_guard；package/service 命令注入；abspath 非 realpath + /LINGOS/system 不在保护前缀；file_write 可覆写 ai_permissions/auth_always/mcp_servers；session 归档路径穿越；截断丢 system 消息；每轮重复插目标；g_events 跨会话；子 AI 抢事件；sessions/tasks 非原子写；授权轮询 socket 泄漏；8088 单线程/无长度上限；语义检索 O(N) embed；sync_full 泄露 api_key/HA token；MCP token 0644；MCP SSRF；工具参数明文日志；记忆无脱敏；vision_train 假训练；98 条 ext 无闸门；ha_control 默认放行；会话读无归属；agent_runtime 空能力放行。
- **发行版 P2/P3**：waybar 配置 schema 错误；/lib64；GRUB timeout；VNC 5900 与 vnc:false 冲突；webui themes.css/theme.js 404 + Google Fonts；ai-local 无模型；power-menu 依赖未安装；geoip 读 /etc/timezone；__pycache__ 提交；verify-claims 过时；validate-syntax 不校验 sessions；build-container 提示 0.9.0；文档仍写 labwc/waybar/tty1。

## 6. 与计划/README 不符

- 0.10 计划的 8 条验收（双架构、Vectras 到桌面、远程修改全 403、L0–L4 矩阵、沙箱越权 0、审计/仪表盘/恢复、文档、SHA256）多数未满足。
- 0.11 的 A/B、dm-verity、可验证启动、声纹、权限中心、LUKS 默认基本未开始。
- 0.12 的本地模型、工具闸门接入、插件签名/沙箱、事件/文件索引未落地。
- README 声称“高风险需审批”“权限闸门/本地优先”“增量更新可用”“生命线不降级”，代码均不成立或为零调用。

## 7. 0.12.1 修复顺序

### 立即（安全/首启阻断）
1. WS key 长度校验；WS/HTTP 默认绑 127.0.0.1，LAN 需显式 token。
2. token 严格解析 + 常数时间比较；文件端点 realpath/O_NOFOLLOW，禁写 /LINGOS/bin|lib|python|system。
3. 8088 绑 127.0.0.1；alert/vision 端点复用 _check()；crisis 二次确认/超时。
4. OOBE apply 末尾延迟 restart lightdm。
5. 源码入库：提交 0.10–0.12 工作区，统一 tag/main。

### 0.12.1
6. rsync 接受 rc24；live LightDM drop-in；手动/live TTY。
7. 构建权限 0644/0755；策略文件 0644 或 0640 root:lingos。
8. ai-tool-gate 按 id 索引；L2+ 放行 853/DHCP/NTP；security-settings 修 loginctl 并触发 apply。
9. build.yml 读 VERSION；合并 Release 双写；统一合并说明文件名。
10. 加密安装 GRUB cryptodisk/pbkdf2（若保留）。
11. CI 冒烟到 stage7/full，去掉 || true。

### 0.13
12. 统一审批模型（MCP/插件/子 AI/perm_set/set_auto_allow）。
13. 备份/回滚、更新签名、TLS、摄像头单属主、并发/资源限制。
14. 清理未编译/重复模块；文档回归真实。

## 8. 最小补丁要点

- WS：`ws_accept` 校验 key 长度为 24（RFC 6455），缓冲改为 `[24+36]`；调用处非法 key 直接 close。
- token：cJSON 解析 tokens.json，逐条 `strcmp`（可用常数时间比较），校验 expires/revoked；文件 0600。
- 文件端点：`realpath` 前缀白名单；禁写可执行目录；上传 `O_CREAT|O_EXCL|O_NOFOLLOW` + 临时文件 rename。
- 8088：`HTTPServer(("127.0.0.1", AUDIO_HTTP_PORT), ...)`；alert/vision 分支开头 `if not self._check(): return`。
- OOBE：`nohup sh -c 'sleep 1; systemctl restart lightdm' >/dev/null 2>&1 &`。
- 权限：构建统一 `install -m 0644/0755`；security-policy.json 改 0644 或 0640 root:lingos。

## 9. 验证清单

- 静态：`scripts/validate-syntax.sh`；`bash -n`；`python3 -m py_compile`；JSON/YAML。
- 单测：agent_runtime 7、local_suite 50、chat_contract；新增 WS/token/files/8088/crisis 负向用例。
- 构建/真机：Vectras arm64 自动安装到 OOBE 再到 Cinnamon；LUKS 解锁；live 不启 LightDM；手动/live/diag 菜单可交互。
- 安全：未认证 WS 超长 key 被拒；`Bearer token` 不再通过；上传 /LINGOS/bin 被拒；LAN 8088/8080/2939 默认不可达。
- 【需实测】Cinnamon/muffin 在无 mesa/llvmpipe 的 TCG 环境能否建 GL；OOBE 循环是否复现；live 用户/sudoers 残留是否可登录。

## 10. 继承 / 创新 / 禁止改动

- **继承**：Ubuntu noble live-build、arm64 grub-mkrescue、Cinnamon/XFCE/LightDM、登录前 OOBE、10 阶段安装器与固定标记、L0–L4 语义、审计哈希链格式、轻量 ISO + 功能包、发布分片/SHA256。
- **创新**：先修 P0 安全/首启；源码入库与 CI 门禁；权限矩阵统一；隐私开关/策略真实执行；AI/插件运行时接入。
- **禁止改动**：安装器 CLI 与固定标记、OOBE 完成标记/密码边界、审计格式、发布分片流程、用户数据 /LINGOS/data；不自动下载/构建/发布。

## 11. 待实测

- Cinnamon/muffin GL（mesa/llvmpipe）；OOBE 循环；LUKS 启动；live 用户残留；Vectras 30s 启动目标；RAM/ISO 体积。
