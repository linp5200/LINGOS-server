# LING OS Web 桌面（webdesk）

生产前端，零构建：`index.html` + `assets/app.css` + `assets/app.js`，由 `services/lingos_web/server.py` 直接托管（默认 8080，同源，无 CORS）。

## 运行

```bash
# 在仓库根目录
python3 services/lingos_web/server.py --root /tmp/lingos-data --web webdesk --port 8080
# 首次打开会要求创建管理员（≥8 位密码），成功后显示一次性 root key
```

Termux 里由 `lingos` 脚本启动，见 `docs/LINGOS-0.13-用户指南.md`。

## 结构

```
index.html          外壳：启动/错误屏、登录、导航壳、七个视图容器、确认条
assets/app.css      设计令牌 + 组件 + 竖屏/横屏布局
assets/app.js       全部前端逻辑（分 6 段，见文件头注释）
assets/icon.svg     PWA 图标
manifest.json       PWA（可添加到主屏幕）
```

## 路由与视图

| hash | 视图 | 数据来源 |
|---|---|---|
| `#/home` | 家宅防线 | `/api/devices` `/api/events` `/api/security` |
| `#/devices` | 设备 + 场景 + 详情 | `/api/devices`、`/api/devices/{id}/command`、`/api/devices/discover` |
| `#/files` | 文件 | `/api/fs/list` `/api/fs/read` `/api/fs/write` `DELETE /api/fs` |
| `#/security` | 安全 + ACL + 用户 | `/api/security` `/api/acl` `/api/users` `/api/security/level` |
| `#/ai` | 管家 | `POST /api/chat` |
| `#/system` | 系统命令 | `POST /api/cmd`（转到 127.0.0.1:18080） |
| `#/settings` | 设置 | `/api/settings` `/api/token` `/api/backup` `/api/restore` |

## 布局

- `body[data-layout]` 由 `detectLayout()` 按高矮判断：竖屏（手机）用底栏 + 单列，横屏（电脑）用侧栏 + 多栏。
- 摄像头预览默认关（`localStorage.lo-preview`），只有开关打开且设备在线、`state.stream` 非空才渲染。
- 主题日/夜两套令牌，`localStorage.lo-theme`，head 内预置避免闪白。

## 约定

- 没有后端 → `err-noserver`；没有命令服务 → 明确 502；没有 AI socket → 显示后端返回的原文。**任何情况下不伪造数据。**
- 危险操作（开锁、切安全等级、删用户/设备、恢复备份）必须二次确认。
- 请求统一 8s 超时；任一接口 401 回登录页。
- 新增入口优先挂 `data-act` / `data-sys` / `data-cmd`，由 `app.js` 里的统一委托处理。

## 校验

```bash
node --check assets/app.js
# 浏览器里：七视图无横向溢出、无 console error（401/502 属预期探测）
```

打磨计划与缺口见 `docs/LINGOS-Web-前端打磨计划.md`，功能与后端对照见 `docs/LINGOS-Web-功能地图.md`。
