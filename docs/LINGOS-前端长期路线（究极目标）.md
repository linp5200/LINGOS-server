# LING OS · 前端长期路线（究极目标版）

> 2026-11-14 先生要求：不只为第一版，按“通用型系统”的长期目标把 UI/前端该具备的能力查全，为后续铺路。
> 本文是检索 + 推演，**未部署**；标注“仅摘要/未读”的不冒充全文结论。

## 零、定位

LING OS 是通用系统（不是单一行业 App）。UI 长期目标不是“做一个好看的页面”，而是：

1. **可复用**：一套 token / 组件 / 模式，手机、桌面、平板、未来形态都能用。
2. **可访问**：不同能力、不同输入方式的人都能用。
3. **可演进**：主题、语言、密度、动效、AI 能力都能换，不用推翻重写。
4. **可信**：AI 与自动化有边界、有确认、有审计、可撤销。

下面按能力分层，每一层给“查到什么 → LING OS 怎么用 → 什么时候做”。

---

## L1. 设计系统内核（基础中的基础）

**查到**
- 设计 token 已有 W3C DTCG 首个稳定版规范（2025-10 公告）：`$type`/`$value`，颜色、尺寸、字体等可跨工具交换。
- 组件必须定义状态矩阵：default / hover / focus-visible / active / disabled（含原因）/ loading / error / selected。
- 设计系统成熟度通常按“有无 token → 组件库 → 文档 → 治理/采纳 → 度量”分级（maturity model，抓取失败，按通用模型记录）。

**LING OS**
- 现在：`app.css` 的 CSS 变量已经是 token 雏形；组件集中在 `app.js` 的 render 函数。
- 下一步：把颜色/间距/字号/圆角/动效导出成 `tokens.json`（DTCG 格式），CSS 从 token 生成；组件状态写成一份 `components.md`。
- 后续：视觉回归（截图 diff）+ 组件 API 稳定；新增组件必须先有状态矩阵，不然不许进。

**时机**：0.15 起；不阻塞当前流畅度。

---

## L2. 多端与自适应

**查到**
- CSS 容器查询（container queries）已可用：按**组件所在容器**宽度适配，而不是只看视口；适合卡片在不同栏位复用。
- Android 响应式导航：窄屏底部导航、较宽用 navigation rail；断点通常在 600dp 左右开始考虑。
- 安全区：`viewport-fit=cover` + `env(safe-area-inset-*)`；底部 Tab/输入条必须避让。
- 折叠屏有 posture / viewport segments API（Chrome 试验阶段），可按铰链分栏。

**LING OS**
- 现在：`body[data-layout=portrait|landscape]` + 媒体查询；已处理底部 Tab 和终端底部。
- 下一步：把卡片改成 `container-type: inline-size`，用 `@container` 决定单列/双列；断点从 600/900/1200 三档开始，不追每个机型。
- 后续：折叠屏分栏（左列表右详情）；平板横屏用 rail；桌面支持窄窗口（窗口 <560 时退回底部 Tab）。
- 输入方式：鼠标 hover、键盘 focus、触屏 active、手写笔 hover 都要覆盖；`@media (hover:hover)` 区分。

**时机**：0.15 布局审计时一起做。

---

## L3. 主题与无障碍（长期主线）

**查到**
- WCAG 2.2 仍是当前可验收标准；WCAG 3.0 还是 Working Draft（2026-10 的 Requirements 仍是 Group Note），**不能拿 3.0 当验收线**，但可按它的方向（认知、低视力、任务级）提前设计。
- `forced-colors: active`（Windows 高对比度）会强制系统色、去掉阴影/渐变；不要为它另做一套设计，只做小修补（如用 border 代替 box-shadow）。
- `prefers-contrast`、`prefers-reduced-motion`、`prefers-color-scheme` 三个偏好都要尊重。
- 焦点、触控、对比、键盘、屏幕阅读器是基础；认知无障碍（简明语言、少打断、可撤销）是下一层。

**LING OS**
- 现在：日/夜 token、reduced-motion、焦点环、toast role、模态 inert/焦点陷阱已做。
- 下一步：
  - 增加“高对比/强制色”适配：`@media (forced-colors: active)` 下用系统色 + 边框，不依赖背景色区分状态。
  - 对比度自动检测脚本（Playwright 取值算对比），日/夜都跑。
  - 键盘全流程走查（Tab 顺序、Esc、焦点归还）；TalkBack 实测。
  - 认知层：危险操作可撤销/二次确认、长任务可暂停、错误可恢复。
- 后续：WCAG 3 方向的任务级验收（不是逐条打勾），以及“减少动效/减少干扰/大字”三个用户偏好。

**时机**：0.15 做高对比+对比脚本；语音/屏幕阅读器实测随后。

---

## L4. 国际化（i18n）

**查到**
- RTL 布局要镜像：方向性图标、边距、进度方向；不能只翻译文字。
- 伪本地化（pseudo-localization）能在开发期暴露截断/硬编码。
- CJK 与拉丁的字体、行高、标点规则不同；日期/数字/货币要按 locale 格式化。
- 文案长度差异会撑破按钮/卡片，需要弹性布局与截断策略。

**LING OS**
- 现在：setup 有 `language` 字段，但只有 `lang` 标记，没有 i18n。
- 下一步：先做 **zh-CN 文案集中化**（一个 `strings.js`），避免散落在 render 里；英文作为第二语言验证布局弹性。
- 后续：RTL 语言（阿拉伯语）只作为布局压力测试，不承诺完整翻译；日期/文件大小/时间用 `Intl`。
- 伪本地化：把中文替换成加长 30% 的假串，跑截图看溢出。

**时机**：0.16；先做文案集中化，不急着翻译。

---

## L5. 性能与感知（长期）

**查到**
- Core Web Vitals：INP 目标 ≤200ms；长任务 >50ms 要拆；大 DOM/复杂布局会拖慢交互（[web.dev INP](https://web.developers.google.cn/articles/optimize-inp?hl=zh-cn)）。
- 转场/动画只用 transform/opacity；`will-change` 临时加、结束删（[MDN](https://developer.mozilla.org/en-US/docs/Web/CSS/Reference/Properties/will-change)）。
- 大列表用虚拟化/分批；骨架尺寸与内容一致，CLS <0.05。
- 低端设备/WebView 是真实下限，实验室数据不能代替实机。

**LING OS**
- 现在：单页 `innerHTML` 重绘；事件/内核列表可能上百项。
- 下一步：切页保留 DOM（或至少复用列表节点）、长列表分批渲染、首屏只 stagger 前 5 项；切页采样 INP/CLS。
- 后续：列表虚拟化；后台轮询合并（visibilitychange 暂停）；离线缓存标注“可能过期”。
- 预算：JS 首屏 gzip <120KB（当前 app.js 约？需量）、单次交互主线程 <50ms。

**时机**：0.14 流畅度批次就做基础；虚拟化 0.15。

---

## L6. AI 原生交互（通用系统的关键差异）

**查到**
- Microsoft HAX 18 条：明确系统能做什么/多可靠、按场景给时机、相关推荐、避免偏见、高效调用/关闭/纠正、不确定时缩小范围、解释原因、记住近期交互、谨慎学习、细粒度反馈、说明后果、全局控制、变更通知。
- 可信 AI 设计（综合 NNG/HAX/PAIR/EU AI Act）：
  - **信任校准**：既不盲信也不全验；显示置信度/来源/局限。
  - **三层护栏**：行动前（澄清意图/确认高风险）、行动中（进度/暂停/取消）、行动后（复核/撤销/反馈）。
  - **Agent 三原则**：Control（随时停/改/取消）、Consent（有后果的动作先批准）、Accountability（完整审计）。
  - **风险分级**：低风险自动+日志；中风险建议+批准；高风险强制确认/二次认证。
  - **错误恢复**：承认、解释、给修正、说明防复发、临时加监督。
  - **反模式**：AI 疲劳、AI slop、不必要的 AI、AI washing、强制 AI 流程。
- 流式输出：aria-live 缓冲完整句，不要逐 token 念；提供“读完整回答”；生成中可停止。

**LING OS**
- 现在：AI 页是聊天 + 后端 provider；没有置信度、没有 agent 动作审批、没有审计联动。
- 下一步（0.17 方向）：
  - 消息标注“AI 生成，可能出错”；显示 provider/模型；失败给可执行建议。
  - 流式输出 + 停止按钮；完成后 aria-live 播报。
  - 任何“执行设备命令/改设置”的 AI 动作，先走确认条，写审计；提供撤销（能撤销的）。
  - 全局 AI 开关与范围控制（哪些数据可读、哪些设备可控）。
  - 反馈：赞/踩/纠正，落库但不伪造。
- 后续：生成式 UI（AI 产出卡片/表单）必须限制在白名单组件内，不能执行任意 HTML；高风险动作强制二次确认。

**时机**：0.17；先把聊天做稳，再上动作与审计。

---

## L7. 数据可视化

**查到**
- 图表无障碍：不能只靠颜色；要有文本摘要/数据表替代；色盲友好调色板；坐标轴/单位可读（GOV.UK charts 等实践）。
- 仪表盘：先结论后细节；一张图只表达一个观点；避免 3D/装饰。

**LING OS**
- 现在：没有真正的图表页（首页是数字卡片）。
- 下一步：如果做趋势（温度/电量/事件量），用 CSS/SVG 简单折线 + 数据表切换；颜色 + 形状/标签双编码。
- 后续：图表库只选可访问性好的；提供“下载 CSV/查看数据表”。

**时机**：有真实数据需求再做，不先造图表。

---

## L8. 设计运营与度量

**查到**
- 设计系统成熟度：token → 组件 → 文档 → 治理/采纳 → 度量（maturity model，抓取失败，按通用模型）。
- 可用性度量：SUS（10 题量表）、HEART（ happiness/engagement/adoption/retention/task success）、任务完成率/时间/错误率。
- 质量门：视觉回归、a11y 自动检查（axe）、性能预算、文案 lint。

**LING OS**
- 现在：靠人工截图和审查子代理。
- 下一步：仓库加 `npm run check:ui`（Playwright 截图 diff + axe + 对比度 + 溢出检查）；每个 PR 跑。
- 后续：组件文档站（本地静态页）；设计决策记录（ADR）；SUS/任务测试每版本一次（找 3–5 人）。

**时机**：0.15 起逐步加，不一次性上全套。

---

## L9. 未来形态（只铺路，不现在做）

| 形态 | 查到/判断 | 铺路方式 |
|---|---|---|
| 折叠屏/双屏 | posture、viewport segments（试验） | 布局用容器查询 + 可分栏的列表/详情结构 |
| 平板/桌面 | rail + 多栏 | 断点 600/900/1200；窄窗口回退 |
| TV/10-foot | 焦点驱动、大字号、遥控器方向键 | 所有交互可键盘/方向键完成；焦点可见 |
| 语音 | 语音输出需转写、可打断、语速控制 | AI 回复保留文本版；语音作为输入/输出层，不替代 UI |
| AR/VR/XR | WCAG 3 明确要覆盖 | 现在不投入；保持 2D 信息架构清晰，未来可映射 |
| 可穿戴 | 极小屏、 glanceable | 核心状态用一句话/一个数字表达（现有数字卡片可复用） |

---

## 分阶段建议

| 版本 | 主题 | 内容 |
|---|---|---|
| 0.14（现在） | 流畅度 + 基础审计 | 转场、主题扩散、骨架交接、按压反馈、列表节制、控制台收口、8pt/对比/44px 审计 |
| 0.15 | 设计系统 + 多端 | tokens.json（DTCG）、容器查询、rail/断点、forced-colors、对比自动检测、组件状态文档 |
| 0.16 | i18n + 可视化 | 文案集中化、伪本地化、Intl 格式化、图表（有需求才做） |
| 0.17 | AI UX | 流式、停止、置信/来源、动作确认、审计、AI 开关与范围、反馈 |
| 1.0 前 | 设计运营 | UI 质量门（截图/axe/对比/性能）、组件文档、SUS/任务测试 |

## 需要补的实机验证（不查清楚会返工）

1. 手机 WebView/Chrome 版本 → View Transitions、container queries、forced-colors 支持情况。
2. 真实 safe-area 值、软键盘弹起时 Tab/输入框位置。
3. TalkBack 对 toast/模态/图标按钮的朗读。
4. 夜间 token 逐项对比度。
5. 低端机（这台手机）切页 INP/掉帧。

## 来源

已读正文/摘要：
- [WCAG 3 Requirements（2026-10 Group Note）](https://w3c.github.io/wcag3/requirements/)
- [Microsoft HAX Guidelines](https://www.microsoft.com/en-us/haxtoolkit/ai-guidelines/)（18 条正文在 Design Library，未逐条展开）
- [AI Safety, Trust Calibration, and Guardrail Design Patterns](https://raw.githubusercontent.com/phazurlabs/sumi/main/skills/agentic-ai-generative-ux/references/ai-safety-trust-guardrails.md)（综合 NNG/HAX/PAIR/EU AI Act）
- [MDN Container queries](https://developer.mozilla.org/en-US/docs/Web/CSS/Guides/Containment/Container_queries)
- [MDN forced-colors](https://developer.mozilla.org/en-US/docs/Web/CSS/Reference/At-rules/@media/forced-colors)
- [web.dev 优化 INP](https://web.developers.google.cn/articles/optimize-inp?hl=zh-cn)
- [Next.js view transitions](https://nextjs.org/docs/app/guides/view-transitions.md)
- [Choreography Recipes](https://raw.githubusercontent.com/emilkowalski/skills/main/skills/choreograph/RECIPES.md)
- [Microcopy Reference](https://raw.githubusercontent.com/danilonovaisv/portfolio-danilo-final/main/.claude/plugins/ghost-design-system/skills/claude-design-auditor/references/microcopy.md)
- [Apple HIG Typography（JSON）](https://developer.apple.com/tutorials/data/design/human-interface-guidelines/typography.json)
- [W3C DTCG 稳定版公告](https://lists.w3.org/Archives/Public/public-design-tokens/2025Oct/0003.html#1)
- [MDN will-change](https://developer.mozilla.org/en-US/docs/Web/CSS/Reference/Properties/will-change)
- B站：[6 个 UI 设计技巧（字幕已读）](https://www.bilibili.com/video/BV1cKpee7E2a)、[UI 高级感=精度（字幕已读）](https://www.bilibili.com/video/BV1SFWr6oEeC)

仅摘要/未读：Google PAIR（页面抓取失败）、Design System Maturity Model（抓取超时）、RTL 规范、图表无障碍具体规范、折叠屏 API（仅标题）。这些在对应阶段再查，不当作已定结论。
