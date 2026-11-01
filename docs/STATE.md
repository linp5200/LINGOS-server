# LING OS 项目状态

> 更新时间：2026-11-01
> 当前目标版本：0.10.3（接着 0.11、0.12）

## 已交付
- 0.10.2 正式版（v0.10.2，arm64，2026-10-31）：桌面应用、VNC/X11 兜底、LUKS2、隐私脚本、OOBE 雏形、Plymouth/picom、安全中心只读、Agent Runtime 打包
- 本地 ISO：/root/Downloads/lingos-v0.10.2/lingos-installer-aarch64.iso（sha256 f59975e4…）

## 0.10.2 已知问题（先生实测）
- 安装 stage 7/10 复制约 30 分钟，进度刷屏/ETA 不准
- 控制台中文无 CJK 字体显示异常
- 首次启动 OOBE 未出现；疑似又从 live ISO 启动（tty1 user 自动登录）
- 桌面只有壁纸，无面板/图标/开始菜单

## 0.10.3 进行中
- 桌面切换：Cinnamon 默认 + XFCE 轻量/救援（LightDM 选择会话）
- OOBE 改为 LightDM 首次自动登录临时 oobe 会话 → 配置用户/密码/语言/时区/隐私 → 注销回登录界面
- 安装器：英文/ASCII、真实进度、复制优化（子智能执行中）
- 保留 Plymouth、picom、壁纸、隐私设置入口、安全中心

## 后续
- 0.11：安全隐私核心（权限中心、防火墙/DNS、沙箱、审计面板、恢复模式）
- 0.12：AI 助手与插件框架（本地模型优先、工具闸门、插件签名/沙箱）

## 验证约定
- 新增功能先通过 scripts/validate-syntax.sh（shell/Python/JSON/YAML/Desktop Entry + 单测）
- 只有静态校验与构建冒烟通过后才发布；避免反复下载
