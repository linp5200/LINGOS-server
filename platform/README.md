# LING OS 平台抽象与能力矩阵（0.13）

业务代码不直接依赖 nftables/systemd/seccomp/设备节点；通过 platform 接口选择实现：

| 接口 | termux | linux | vm |
|---|---|---|---|
| firewall | 应用层代理/白名单 | nftables | nftables |
| sandbox | 进程/目录隔离 | seccomp+cgroup+namespace | 完整 |
| devices | Termux:API/网络设备 | /dev 直通 | 虚拟设备 |
| display | Web/PWA | Cinnamon/X | Web/VM |
| update | rootfs tar | A/B/包 | 镜像 |
| power | Android 生命周期 | systemd | VM |

0.13.0 先落 Web/设备/ACL；0.13.1 设备适配；0.13.2 ACL；0.13.3 稳定性。
Termux 不可用能力保留 linux/vm 实现，未来 ISO/VM 直接复用。
