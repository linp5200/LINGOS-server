# LING OS arm64 本地安装记录（0.13.0）

> 2026-11-14 · 先生要求"下载到本地并安装一下(arm)"。
> 本机：aarch64 Linux（Ubuntu 25.10，glibc 2.41），非 Termux。

## 一、下载

| 项 | 值 |
|---|---|
| 来源 | GitHub Actions build-bundle run `43467370367` 产物 `LINGOS-arm64` |
| 压缩包 | `LINGOS-arm64.zip`，45 MB |
| SHA256 | `54d0a9acdfca97ce2e71ff43a5afa38703dc2d86f102e1f639e554cc8ffb0846` |
| 全捆包 | `LINGOS_server_linux_v0.13.0_aarch64_allbin.tar.gz`（36 MB） |
| 插件包 | `..._plugin.tar.gz`（72 KB） |
| 系统包 | `..._sysbin.tar.gz`（11 MB） |

## 二、安装

```bash
# 备份已有配置/状态
tar czf ~/Downloads/lingos-backup/pre-arm64-install-20261114-0053.tar.gz -C /LINGOS system/config state data

# 用包内 install.sh 安装到 /LINGOS（保留已有 system/config）
cd /root/Downloads/lingos-install/LINGOS_server_linux_v0.13.0_aarch64_allbin
bash install.sh /LINGOS
```

安装内容：9 个二进制（lingos_linux / lingosd / lingos_supervisor / alertd / visiond / voiced / monitor / ai_vision / lingosctl）、lib/、python/（3.10.12 venv）、share/webui、48 个 Python Agent 脚本到 `/LINGOS/bin/`。

同时把本仓库的新前端部署到 `/LINGOS/share/webui/`（旧版留 `/LINGOS/share/webui-legacy/`）。

## 三、启动与验证

```bash
cd /LINGOS && ./start.sh
# 另起 Web 桌面（新前端）
python3 services/lingos_web/server.py --root /LINGOS --web /LINGOS/share/webui --port 8080
```

| 检查 | 结果 |
|---|---|
| `lingos_linux --version` | `LING OS LN-0.9.0` |
| `lingosctl --help` / `--list` | 正常，`api.describe` 返回 9 条 `api.*` 命令 |
| 包内 Python | 3.10.12 可用 |
| 守护进程 | lingos_linux / lingosd / lingos_alertd 起来；AI server 监听 `/LINGOS/run/ai.sock`；语音 8088 |
| registry.json | 首次启动后由守护生成到 `/LINGOS/registry/core/registry.json` |
| Web 桌面 | `http://127.0.0.1:8080` 新前端可用，登录、设备、设置、系统页正常（截图 `installed-*.png`） |
| AI provider | 经真实 ai.sock 读到 `ollama`（base_url `http://127.0.0.1:11434`），key 脱敏 |
| 事件/设备 | /LINGOS 既有数据加载正常（5 台设备，3 在线） |

## 四、发现的问题（按严重度）

1. **`/api/cmd` HTTP 通道 body 丢失（18080）**：请求体到不了 AI，返回
   `{"status":"error","content":"Invalid JSON: Expecting value: line 1 column 1 (char 0)"}`。
   已在 Web 端加 `/run/daemon.sock` 兜底（lingosctl 同协议
   `{"id":1,"command":...,"args":{},"rely":true,"return":"json"}`），系统页在 HTTP 通道坏掉时仍可用。
   C 侧 `upload_handler`（MHD 上传回调）需要单独查。
2. **86 条点分命令在本 bundle 未注册**：`lingosctl system.status` 返回 `-1002 command not registered`；
   只有 9 条 `api.*`。`registry.json` 存在但内容不含点分命令。需确认注册链
   （`src/shell/dotted_cmd.c` 是否只进 `lingos_linux` UI 进程、不进 lingosd）。
3. **Ollama 默认地址写成了 8080**（Web 端口）→ AI 永远连到 Web 服务器。已改为
   `http://127.0.0.1:11434`（`src/python/ai_server.py` 两处默认值 + 本机 `/LINGOS/system/config/ai_config.json`），
   重启后日志确认 `ollama url: http://127.0.0.1:11434`。
4. 预期内噪音：MQTT 连接拒绝（无 broker）、weather TLS 到 api.wolfx.jp 失败（外网/证书）、
   `ensure_ai_server_running attempt 1/4 failed` 偶发（AI 随后成功）。

## 五、回滚/停止

```bash
# 停守护
pkill -f '^/LINGOS/bin/lingos_linux$'
pkill -f '^/LINGOS/bin/lingosd$'
# 恢复旧配置
tar xzf ~/Downloads/lingos-backup/pre-arm64-install-20261114-0053.tar.gz -C /LINGOS
# 旧 Web 前端
mv /LINGOS/share/webui /LINGOS/share/webui-new && mv /LINGOS/share/webui-legacy /LINGOS/share/webui
```

## 六、结论

arm64 全捆包**可以安装、可以启动、Web 桌面可用**；但 C 命令面在本机安装形态下有两处硬缺陷
（HTTP 命令代理丢失 body、点分命令未注册），不影响 Web/AI 的 Python 侧使用。建议在 0.13.3 收尾时修。
