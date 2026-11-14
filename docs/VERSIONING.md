# 版本策略

先生原则（2026-11-14）：**每次推送都要换版本；下载到的每个包必须是不同版本。**

## 单一事实源
- `VERSION`（例如 `0.13.1`）是发行版本号唯一来源；`Makefile` 的 `LN-*` 与 Web 的 `/api/health.version` 都从它或构建注入读。
- 每次推送前先 bump：

```bash
scripts/bump-version.sh patch   # 0.13.1 -> 0.13.2
```

## CI 产物命名
- build-bundle 全捆包：`RELEASE_VER = $(cat VERSION)+<short-sha>`，包名形如
  `LINGOS_server_linux_v0.13.1+2578597_aarch64_allbin.tar.gz`。
- rootfs：`LINGOS_rootfs_arm64_<VERSION>+<short-sha>.tar.zst`，包内 `/LINGOS/version` 写同一版本。
- 因此即使 VERSION 忘记 bump，不同提交的产物文件名也不会相同；bump 后版本号本身也前进。

## 运行时显示
- Web：`/api/health.version` 读 `LINGOS_VERSION` → 仓库 `VERSION` → `/LINGOS/version`。
- Termux launcher 把 `rootfs/LINGOS/version` 导出为 `LINGOS_VERSION`，Web 与 UI 显示同一版本。

## 验收
- `sha256sum` 与包内 `.sha256` 一致；
- `cat /LINGOS/version` 与包名里的版本一致；
- 同一 VERSION 的两次构建，包名仍因 `+<sha>` 不同。
