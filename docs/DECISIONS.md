# LING OS 关键决策

1. 桌面：Cinnamon 默认，XFCE 轻量/救援；KDE 不作为默认（Plasma 6.8 Wayland-only 与 Vectras 不兼容）。
2. OOBE：登录前，由 LightDM 首次自动登录临时 oobe 会话完成账户/密码/语言/时区/隐私配置，完成后注销回正常登录；保留 TUI 回退。
3. 安装器：控制台阶段英文/ASCII；显示真实阶段/进度/ETA；失败可进调试 shell；中期评估 Calamares。
4. 打包：默认轻量 ISO + 可选完整功能包；避免反复全量下载。
5. 安全：隐私等级 L0–L4、本地唯一修改、哈希链审计、AI 工具/插件权限闸门、应用沙箱。
6. 发布：arm64 优先；x86 构建链修复前 include_x86=false；发布前静态校验 + 冒烟告警。
