# LING OS 0.15 完成记录（UI 系统层）

> 2026-11-14 · 目标：把 0.14 流畅度收口 + 0.15 设计系统/多端/无障碍基础落地。
> 数据规则：正式版无假数据；预览版仍单独放 `lingos-preview/`，全部 `PVnodata`。

## 已完成

### 流畅度
- 路由切换：`board.view-swap` 交叉淡入（220ms），前进/后退方向由 `data-navdir` 决定；不依赖 View Transition API，旧 WebView 也能用。
- 主题切换：`startViewTransition` 圆形扩散（320ms）；回调未执行时 120ms 兜底，主题一定切换。
- 骨架→内容：`contentIn` 180ms；列表只前 5 项 stagger（40ms/项）。
- 按压反馈：按钮/卡片/行 90ms scale .985。
- 控制台：移除系统页按钮；仅关于页版本连点 3 次（3 秒窗口、44px 命中、成功后自动滚到工作台）。

### 设计系统
- `webdesk/assets/tokens.json`（DTCG：color/space/radius/duration/typography），`scripts/build-tokens.py` 生成 `tokens.css`（含既有 `--paper` 别名）。
- `docs/组件状态与规范.md`：状态矩阵、尺寸、反馈选择、验收命令。

### 多端/主题/无障碍
- 容器查询：`.cards`/`.dev-grid` 按容器宽度 720/1080 自动 2/3 列。
- `forced-colors: active`：系统色 + 边框，不靠背景区分；`prefers-contrast: more` 加深线色。
- 夜/日对比度检查脚本 `tests/ui_check.mjs`（日 + DARK=1 两跑，均 0 问题）。
- Service Worker 注册（PWA 离线入口）。

### 后端补口
- 文件上传：`fsFile` 读文本（≤512KB）写 `/api/fs/write`，失败给可读错误。
- 插件启停：`POST /api/plugins/toggle`（admin、原子写 0600、事件记录），插件页启停按钮。
- 修：`/api/chat` 的 `sqlite3.Row.get` 崩溃；`skill_handlers` 硬编码 `/LINGOS` 导致原生 AI 启动崩；插件 toggle 重复读请求体导致挂起。

## 验证
- 单测：22 项通过（含插件启停 admin 200/普通用户 403/未知 404）。
- Playwright：VT 支持、tokens 加载、主题切换、切页、版本连点、终端、上传，无页面错误。
- `ui_check.mjs`：390×844 日/夜，7 视图，0 溢出 / 0 小触控 / 0 对比度问题。

## 未做 / 后续
- i18n（文案集中化）、图表、AI UX（流式/置信/动作审批）、虚拟列表、视觉回归 CI、TalkBack 实测。
- 夜间对比度脚本只覆盖代表选择器，未覆盖所有组合。

## 审查与修正（2026-11-14 夜）
- 独立 UI 审查 4 轮：6.5 → 7.4 → 8.3；代码审查：7.5 → 8.2 → 8.9。P0/P1 已清零；剩余为 P2（space/typography token 未消费、SW cache-first 手工版本、部分历史加固项）。
- 修正：tokens 单一事实源（颜色/圆角/时长，夜间 night 组）、容器查询、SW 一次性迁移与注册、版本热区 151×44、动效四档 52×44、骨架 120ms 淡出、导航方向、插件启停 UI、文件上传去重、插件请求校验、23 项测试。
- UI 检查脚本重写：7 视图 + appearance/plugins/about + 终端 + 确认条 + 版本热区 + SW；日/夜均 badCount=0、sw=1。

## 终版（0.15.2）
- 主题切换最终采用“立即换肤 + 280ms 颜色过渡”（不再用 View Transition 圆形扩散；VT 在部分环境会挂起，稳定优先）。
- 清理全部 view-transition 死规则；tokens 增加 display/title 字号与夜间次要色；更多组件使用 space/typography token。
- ui_check：7 视图 + 设置子页 + 终端 + 确认条 + 1440 列数/3 卡探针 + 版本热区 + 主题一致性 + SW 门槛；日/夜 badCount=0、sw=1。
- 独立审查终轮：UI 9.0 / 代码 9.0，P0/P1=0，各剩 3 / 10 条 P2。
