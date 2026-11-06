# LING OS 0.13 完成检查表（推送前逐项核对）

> 规则：全部为 [x] 且静态校验/测试通过，才允许提交/推送；未完成前不推送、不部署。

## 0.13.0 能跑
- [x] VERSION=0.13.0；docs/ROADMAP/STATE/DECISIONS/0.13 计划
- [x] P0：WS 栈溢出、token 严格 JSON、/api/files 禁写、8088 鉴权、默认 loopback
- [x] lingos-web 8080（认证/会话/设备/文件/安全/AI/命令代理）
- [x] Web 桌面（DSH/终末地风格、浅/深色、PWA/全屏）
- [x] rootfs 构建脚本 + Termux 安装/启动脚本
- [x] platform 能力矩阵
- [x] tests/test_lingos_web.py 14 项（含恢复）；validate-syntax 全通过
- [x] rootfs arm64 最终构建（CI run 42242489520，109MB，含恢复 API）
- [x] 9 个二进制全量编译（CI 编译步骤 + rootfs 装配成功）
- [x] Termux 同构 proot 安装脚本 + 8080/setup/设备/文件读写实测
- [ ] 先生实际 Termux 设备最终确认/重启保活

## 0.13.1 能控家
- [x] 设备抽象/管理器；虚拟设备完整
- [x] Home Assistant REST 适配器
- [x] MQTT 适配器（可选 paho-mqtt）
- [x] 米家 miIO 实验接口（python-miio + token）
- [x] 设备添加/删除/发现/命令；适配器状态
- [ ] 真实米家/HA/MQTT 设备联调（需先生设备/网络）
- [ ] 设备发现（mDNS/miIO 广播）与离线事件总线

## 0.13.2 能管权限
- [x] 目录 ACL 后端 + 管理 UI + 用户管理
- [x] 四档安全 UI（关闭/常规/限制/严格 + L4 高级）
- [x] admin/root key（setup 显示一次；高风险操作校验）
- [x] 独立文件管理器（/files.html）
- [x] L3/L4 非管理员文件访问默认拒绝
- [ ] ACL 与设备/应用权限统一模型（App 侧）
- [x] root key 轮换（管理员密码，setup 只显示一次）
- [x] 审计导出（/api/audit/export + 设置按钮）

## 0.13.3 能长期用
- [x] API Token（App/平板）
- [x] 局域网配对码（/api/pair）
- [x] Termux wake-lock/通知启动
- [x] /api/cmd 同权代理（Web/App 同一认证）
- [x] Windows 式最小化/任务栏恢复（视图级；多窗口拖拽待 0.13.3 收尾）
- [x] 通知中心/事件列表/未读 badge
- [x] App WebSocket 2939 token 同步（C auth 帧；真机联调待）
- [ ] 后台保活真机验证
- [x] 备份 API（/api/backup tar.gz）
- [x] 恢复 API（/api/restore，root key + 归档校验 + SQLite 完整性）
- [x] 用户指南（Termux/设备/ACL/备份）
- [x] 恢复 API 与 UI（/api/restore + 设置页恢复备份）

## 最终门禁
- [x] scripts/validate-syntax.sh 通过
- [x] tests/test_lingos_web.py 全通过（14 项）
- [x] C 全量 make 通过（CI 编译 + rootfs 装配）
- [x] rootfs arm64 最终构建 + sha256（CI run 42242489520，109MB）
- [x] 安装脚本实测（Termux 同构 proot）
- [x] 8080 登录→设备→文件读写实测
- [ ] 先生设备最终确认/重启保活
- [ ] 最终合并/推送 main（当前仅 dev 分支用于 CI）
