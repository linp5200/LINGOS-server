# LING OS 0.10.0 实施计划（任务到代码映射）

> 版本：v1（2026-10-30）
> 依据：LINGOS-0.10-安全与隐私架构-完整版、LINGOS-版本与功能规范、LINGOS-工程与工业化规范
> 状态：待执行；下载/发布等待期间编写

---

## 1. 目标与范围

0.10.0 交付：
- 隐私等级 L0–L4 + Custom 模式 + 兼容性提示
- 安全/隐私设置仅本地控制台修改（远程只读、403+审计）
- LUKS2 全盘加密安装（可选但强提示）
- 应用沙箱扩展（seccomp+cgroup+namespace+AppArmor+portal）
- 每应用网络防火墙、审计哈希链、隐私仪表盘
- 离线 GeoIP 粗定位、浏览器隐私硬化、分级信任
- AI 工具/插件限制
- 恢复模式
- 文档/测试/发布齐备

不做（放到后续版本）：dm-verity/A-B 更新（0.11）、声纹身份（0.11）、事件记录（0.12）、指向性（0.13）。

---

## 2. 工作包与代码映射

### W1 隐私等级与策略引擎（S2/S25/S28/S32/S33）
- 现有：src/security/security_config.c、src/config/options.c（privacy_mode 单开关）
- 新增：
  - src/security/privacy_policy.c/.h：等级定义、默认值、等级切换、审计事件
  - src/security/trust_policy.c/.h：网络/位置/设备/来源/文件/工具/插件信任矩阵
  - src/config/schema/privacy-level.json：配置 schema（版本化）
  - webui：隐私仪表盘/等级切换/兼容性提示（只读状态+本地修改入口）
- 接口：/api/v1/security 只读；本地 Unix socket 写（见 W2）
- 验收：L0–L4 每项生效；切换审计；Custom 逐项/按站点/按应用；L4 默认无出站。

### W2 本地唯一配置通道（S26/S27/S30）
- 现有：src/api/http_server.c、src/api/websocket_server.c、src/daemon/syscall_handler.c
- 新增：
  - src/security/local_session.c：检测 seat0/Remote=no/会话类型
  - src/security/security_settings_broker.c：仅本机 Unix socket + 本地会话凭据
  - src/security/policy_integrity.c：策略文件签名/哈希
  - unit/plasma?（桌面安全设置应用）：本地再认证 + 键入确认短语 + 防注入
- 接口：远程修改返回 70002/403 + 审计；本地 socket 0600
- 验收：远程（WebUI/App/VNC/SSH/API）修改全部失败且留审计；模拟点击/自动化被阻止。

### W3 安装器 LUKS2 加密（S3）
- 现有：distro/images/live/lingos-install.sh（分区/格式化/挂载）
- 修改：
  - 分区后 cryptsetup luksFormat（Argon2id，TCG 自适应参数）+ luksOpen
  - mkfs.ext4 在 mapper 上；/etc/crypttab；GRUB 内核参数 cryptdevice
  - initramfs 加入 cryptsetup（MODULES=most + cryptsetup-initramfs）
  - 安装器 UI 增加“全盘加密（推荐）”选项与性能提示
  - 失败回退：加密失败则提示可重试/不使用加密（需二次确认）
- 验收：加密安装→重启→解锁→进桌面；错误密码拒绝；swap 加密。

### W4 应用沙箱扩展（S4/S5，部分 S35）
- 现有：src/security/sandbox/app_sandbox_seccomp.c、app_sandbox_cgroup.c、src/core/app_sandbox.c
- 新增：
  - namespace/bubblewrap 启动封装（bwrap 或内部 unshare）
  - XDG Desktop Portals 接入（文件/摄像头/麦克风/截屏）
  - AppArmor profile 生成/加载；Landlock 文件限制
  - 应用 manifest + 首次授权 UI + 权限使用提示
- 验收：应用默认无网络/无设备；越权用例全部拒绝；portal 授权可撤销。

### W5 每应用网络与系统网络加固（S6/S7/S23）
- 现有：src/firewall/firewall.c
- 修改：
  - nftables 重构：默认 drop、按 cgroup 匹配应用、白名单
  - DoH/DoT 配置、阻断明文 DNS、DNSSEC
  - MAC 随机化（NetworkManager 配置）、关闭 mDNS/LLMNR、IPv6 隐私
- 验收：外部端口扫描全 drop；DNS/WebRTC/IPv6 泄漏测试通过；单应用断网生效。

### W6 审计哈希链与隐私仪表盘（S8）
- 现有：src/security/audit.c
- 新增：哈希链、事件 schema、查询 API、WebUI 仪表盘、导出/清除
- 验收：关键事件可查可导出；篡改检测；日志脱敏。

### W7 GeoIP 与位置（S19/S20/S24）
- 新增：src/security/geoip.c、离线库（签名）、位置权限、生命线告警定位策略
- 验收：天气/预警用城市级；无在线 API 默认；L4 不采集；失败提示不静默。

### W8 浏览器隐私（S21/S22）
- 新增：Firefox user.js/policies、浏览器沙箱、检测页（IP/DNS/WebRTC/IPv6/指纹）
- 验收：默认硬化生效；检测页可查看；无未授权外联。

### W9 分级信任（S32/S33/S36）
- 新增：trust_policy 策略表 + UI + 测试矩阵
- 验收：L0–L4 网络/位置/设备/来源/文件/工具/插件行为符合矩阵。

### W10 AI 工具与插件限制（S34/S35）
- 现有：src/ai/ai_privilege.c、rules_ai_guard.c、plugin 架构
- 新增：tool manifest registry、按等级调用策略、审计；插件签名/沙箱/broker
- 验收：未授权工具拒绝；L4 逐次审批+范围；插件无法越权。

### W11 恢复模式（S10）
- 新增：恢复镜像/菜单、回滚/重置密码/安全擦除、日志导出
- 验收：可进入并完成至少一种恢复操作。

### W12 安全通知中心（S9）
- 现有：alertd
- 新增：安全事件类型、不可静默规则、桌面通知/仪表盘
- 验收：断网/失败场景明确显示，不静默。

### W13 文档/测试/发布
- 文档：用户隐私指南、管理员安全指南、默认值表、接口契约更新
- 测试：越权/泄漏/Sandbox/FDE/恢复矩阵；CI 契约测试
- 发布：0.10.0 Release（双架构、edition 矩阵、SHA256/合并说明）

---

## 3. 实施顺序（里程碑）

M1（基础）：W1 配置/策略 + W2 本地通道 + W6 审计（可先做最小闭环）
M2（安装）：W3 LUKS2 + W11 恢复模式
M3（隔离/网络）：W4 沙箱 + W5 每应用网络
M4（隐私）：W7 GeoIP/位置 + W8 浏览器 + W9 信任
M5（AI/插件）：W10 + W12 通知中心
M6（收尾）：W13 文档/测试/发布

依赖：W1 先于 W9/W10；W2 先于所有“远程只读”验收；W3 独立；W4 与 W5 可并行。

---

## 4. 风险与回滚

| 风险 | 应对 |
|---|---|
| 加密安装在 TCG 慢/失败 | 参数自适应；失败回退非加密需二次确认；保留日志 |
| 沙箱破坏应用兼容 | 默认 L1 兼容模式；按应用放宽；portal 兼容层 |
| 每应用网络误伤 | 白名单 + 一键临时放行 + 日志 |
| 本地通道导致无法远程排障 | 恢复模式 + 只读状态 + 本地确认短语 |
| L4 可用性极差 | 默认关闭；兼容性提示；单点放宽 |
| 审计日志体积 | 轮转 + 摘要 + 可清除策略 |

回滚：配置 schema 迁移备份；版本回退保留上一 ISO；加密安装失败可重装非加密。

---

## 5. 验收标准（0.10.0）

1. 双架构 ISO 启动冒烟 PASS；
2. Vectras arm64 真机：加密安装→解锁→桌面（或 VNC 兜底）；
3. 远程修改安全设置全部 403+审计；本地可改；
4. L0–L4 矩阵测试通过；Custom 生效；L4 默认无出站；
5. 沙箱越权测试 0 高危；应用默认无网络/设备；
6. 审计/仪表盘/恢复模式可用；
7. 文档：隐私指南/管理员指南/契约/测试报告齐全；
8. Release 资产含 SHA256/合并说明（.merge.txt）。
