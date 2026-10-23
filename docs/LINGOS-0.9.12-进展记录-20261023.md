# LING OS 0.9.12 进展记录（2026-10-23）

> 目标：把 0.9（ISO 启动 → 安装 → 进入系统）打通，并让每一步可观测、可测试。

## 一、v0.9.11 黑屏根因（已在 CI 复现）

用 `boot-probe` workflow 下载 v0.9.11 arm64 ISO，在 QEMU UEFI + 串口下复现：

- GRUB 菜单正常显示（VNC/串口都能看到 "LING OS (aarch64)"）；
- 内核与 initrd 正常加载（串口出现 EFI stub / Booting Linux）；
- live-boot 启动后报错：

      BOOT FAILED!
      live-boot will now start a shell. The error message was:
        Unable to find a medium containing a live file system

- ISO 内已确认存在 `/live/vmlinuz`、`/live/initrd.img`、`/live/filesystem.squashfs`
  （iso-inspect workflow 检查），**不是 ISO 内容缺失/损坏**；
- 真机 Vectras 上表现为黑屏 + qcow2 不增长，与 CI 一致。

结论：问题在 live-boot 找不到/挂载不了 live 介质，而不是 ISO 烧录/合并问题。

## 二、本轮修复（0.9.12）

### 1. 启动可观测性

- 内核参数去掉 `quiet splash`，加 `loglevel=7`、`debug`；
- 串口 console 放最后（arm64=`ttyAMA0`、x86=`ttyS0`），`/dev/console` 走串口，
  live-boot/systemd 早期日志可被抓取；
- 新增 initramfs 标记：`LINGOS-INITRAMFS-START` / `LINGOS-INITRAMFS-BOTTOM`
  （确认 initramfs 执行、live-boot 挂载完成）；
- 全部日志同时写 tty0/tty1/串口/kmsg/共享文件夹/网络（见日志方案文档）。

### 2. 安装器

- 10 阶段进度、百分比、ETA，写 `/run/lingos-install/state.json`；
- 修复：`GRUBTARGET` 未定义（会导致 GRUB 安装失败）、`lsblk` TRAN 空串列导致磁盘识别失败；
- 共享文件夹自动探测（Vectras 黑屏时最可靠反馈），网络回传（guest → 10.0.2.2:5555）；
- 安装后创建 `lingos` 用户 + tty1 自动登录 + labwc 桌面配置。

### 3. GRUB 菜单（arm64）

4 个选项：自动安装（默认）/ 手动安装 / 进入 Live（不安装）/ 诊断模式。

### 4. 桌面（0.9 目标“进桌面”）

- ISO 包清单加入 labwc / waybar / wofi / foot / fonts-noto-cjk；
- 安装器创建用户并配置自动登录，`~/.bash_profile` 自动 exec labwc；
- 安装后重启应进入 Wayland 桌面（VNC/QEMU 画面）。

### 5. 测试

- `distro/images/test-boot.sh`：ISO 内容检查 + initrd 组件检查 + QEMU UEFI 启动 +
  串口断言 + 关键标记提前结束 + 定时 VGA 截屏；
- images workflow 构建后自动跑启动冒烟，日志/截图上传 artifact；
- 新增 `boot-probe` / `iso-inspect` 诊断 workflow（跨版本复现/解包检查）。

## 三、当前状态

- [x] v0.9.11 故障在 CI 复现并定位到 live-boot
- [x] 启动日志/进度/菜单/诊断/桌面集成代码完成
- [x] 容器 CI 通过
- [ ] 0.9.12 arm64 完整启动冒烟通过（CI 进行中，QEMU TCG 较慢）
- [ ] v0.9.12 Release 发布
- [ ] 真机 Vectras 验证安装 + 桌面

## 四、后续

1. 启动冒烟通过后发布 v0.9.12（含 SHA256SUMS、分片合并说明）；
2. 真机 Vectras 按《VECTRAS-VM.md》验证：共享文件夹看进度 → 安装 → 重启进桌面；
3. 若仍失败：诊断模式 + 共享文件夹/串口日志定位；优先修 live-boot 介质识别；
4. 0.10 再补：Live 模式桌面、WebUI kiosk、首次启动向导、增量更新等。
