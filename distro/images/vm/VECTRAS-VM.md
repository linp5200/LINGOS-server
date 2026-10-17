# 在 Vectras VM（Android）里运行 LING OS aarch64 ISO

> 适用：Vectras VM 3.9.x（ARM64 手机），虚拟机架构 ARM64。

## 一、前提：ARM64 必须启用 Vectras 的默认 UEFI

Vectras 的 ARM64 固件来自它自带的 `QEMU_EFI.img` + `QEMU_VARS.img`（源码 `FirmwareManager`/`StartVM.java`）。
设置路径：VM 设置 → **Firmware（固件）**：

- ✅ 勾选 **使用默认BIOS/UEFI**（ARM64 的 UEFI 就靠它；"使用UEFI（仅对x64生效）"对 ARM64 无效）
- 自定义参数：清空（避免 `-nographic`/`-cpu`/`-m` 抢参数）
- 共享文件夹：关闭（否则会多一个虚拟盘，安装器自动选盘会把它当候选）
- 内存：≥ 2048 MB（建议 3072）
- 启动自：光驱（首次安装）

启动后 Vectras 显示的 QEMU 参数里应出现：

    -drive file=.../QEMU_EFI.img,format=raw,readonly=on,if=pflash
    -drive file=.../QEMU_VARS.img,format=raw,if=pflash

## 二、硬盘

- 一块 **≥10GB** 的 qcow2 硬盘（Vectras 新建/导入），安装器会自动选"最大且 ≥8G"的盘；10G 够用。
- 不需要加第二块盘。

## 三、ISO

- 需要 **v0.9.5 或更高** 的 `lingos-installer-aarch64.iso`。
- v0.9.0/v0.9.1/v0.9.2 的 aarch64 ISO **不可启动**：live-build 对 arm64 无 bootloader 支持（构建日志明确警告），已改为 arm64 用 grub-mkrescue 重制 UEFI El Torito ISO。
- 下载：https://github.com/linp5200/LINGOS-server/releases

## 四、预期启动流程

    Vectras → EDK2 UEFI（pflash）→ GNU GRUB 菜单（VNC 可见）
    → LING OS 安装器（自动）→ 分区并 rsync 到 qcow2
    → 自动重启 → 从安装盘再次启动时把"启动自"改为 硬盘（或移除 ISO）

ARM64 的 VGA 控制台在 Linux 内核里可能不显示；如果 GRUB 之后黑屏，安装很可能仍在进行，用 VTERM 观察：

    ls -lh /storage/emulated/0/Documents/VectrasVM/roms/<id>/Nowdark.qcow2

qcow2 实际占用从几十 MB 涨到 3–4GB 表示安装正常。

## 五、本地 QEMU（桌面 Linux）等价验证命令

    qemu-system-aarch64 -M virt -cpu cortex-a76 -m 3072 \
      -bios /usr/share/qemu-efi-aarch64/QEMU_EFI.fd \
      -drive if=none,id=cd,format=raw,media=cdrom,file=lingos-installer-aarch64.iso \
      -device nec-usb-xhci,id=xhci -device usb-storage,bus=xhci.0,drive=cd \
      -drive file=lingos-vm.qcow2,if=none,id=disk,format=qcow2 \
      -device virtio-blk-pci,drive=disk \
      -display none -serial stdio

## 六、常见现象

| 现象 | 原因 | 处理 |
|---|---|---|
| UEFI Shell `Shell>` | ISO 无 EFI 引导项（旧版 arm64 ISO） | 换 v0.9.5+ |
| "Guest has not initialized the display" | 未启用 UEFI / ARM VGA 无控制台 | 勾选"使用默认BIOS/UEFI"；黑屏时靠磁盘增长判断安装 |
| 安装器提示"未发现可安装磁盘" | 没有虚拟硬盘 / 只有共享文件夹 | 加一块 ≥10G qcow2 硬盘，关闭共享文件夹 |
