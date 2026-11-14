# LING OS 0.14 · 前端设计总纲（检索版）

> 2026-11-14 先生要求：把 UI / 前端设计相关的内容尽量查全。
> 本文汇总实际检索到的资料，并落到 LING OS 的检查项；**未部署**，供计划和实现使用。
> 标注“仅读标题/摘要”的条目，不冒充读过全文。

## 一、原则（多来源一致）

1. **清晰优先**：文字可读、层级可辨、状态可懂。装饰服从功能。
2. **一致**：同一个概念用同一个词、同一个控件做同一件事；用熟悉模式，不发明图标。
3. **克制**：一个页面一个主焦点；颜色、边框、动效都“刚好够”。
4. **可访问**：对比、触控、键盘、屏幕阅读器、减少动效，不是附加项。
5. **可感知性能**：骨架、即时反馈；慢操作先给回应，不假装完成。

## 二、颜色

| 规则 | 依据 | LING OS 落地 |
|---|---|---|
| 中性灰承担骨架；灰阶只用两端，少用中间 | [B站《6个UI设计技巧》](https://www.bilibili.com/video/BV1cKpee7E2a) | 现有 paper/ink/line 灰阶；新增颜色先问“是否必要” |
| 一个主强调色足够 | 同上 | moss 主强调 + ember 危险/提醒，不再加第三强调色 |
| 正文对比 ≥4.5:1，大字（≥18pt/24px）≥3:1 | 同上 + WCAG AA | 日/夜逐项验；占位符 ≥3:1 |
| 深色模式用色调表面（tone-based surface），不用纯黑 | [Material 3 tone-based surfaces](https://m3.material.io/blog/tone-based-surface-color-m3)（仅读标题/摘要） | 夜间背景 #15180f 系，卡片抬升一层亮度 |
| 语义色固定：错误红、警告黄、成功绿、信息中性 | [SSW toast](https://raw.githubusercontent.com/SSWConsulting/SSW.Rules.Content/main/public/uploads/rules/toast-notifications/rule.mdx) | 夜间也必须保留语义色（已修） |

## 三、字体

| 规则 | 依据 | LING OS |
|---|---|---|
| iOS 默认 17pt、最小 11pt；macOS 默认 13pt、最小 10pt | [Apple HIG Typography](https://developer.apple.com/design/human-interface-guidelines/typography) | 移动端正文 14–16px；最小辅助字 11px |
| 正文 14–16，行高约 1.5 | B站 6 技巧 + HIG | 现在 12.5–13 偏小的地方提到 13–14 |
| 少用字体族；避免 Light/Thin 字重 | HIG | 全站一个无衬线 + mono；字重只用 400/500/600 |
| 长文本每行 45–75 字符 | 通用排版原则（未逐篇核） | 说明文字 max-width 42–60ch |
| 字号变化时保持层级，不整体等比放大 | HIG | 偏好里不做全局字号缩放（后续如需再做） |

## 四、间距与布局

- **8pt 网格**：间距用 4/8/12/16/24/32；4 只做半格（B站 6 技巧）。
- **触控**：WCAG 2.2 最小 24×24 CSS px；LING OS 移动端统一 ≥44×44。
- **安全区**：`viewport-fit=cover` + `env(safe-area-inset-*)`；Tab 栏与终端底部必须让开。
- **行宽**：设置内容区 ≤520–620px；桌面不要拉满 1440。
- **卡片**：圆角、1px 边框、单层阴影；不叠多层阴影。

## 五、组件状态矩阵

每个可交互组件至少定义 8 态（参考 [Component State Matrix](https://raw.githubusercontent.com/compozy/compozy/main/.agents/skills/ui-craft/assets/state-matrix.md)，该页本次抓取超时，规则来自通用实践）：

| 状态 | 表现 |
|---|---|
| default | 基础边框/文字 |
| hover（仅 hover 设备） | 边框加深或背景抬升，≤120ms |
| focus-visible | 2px 焦点环、对比 ≥3:1、不被裁切 |
| active/pressed | scale .985 或背景加深，90ms |
| disabled | 降透明度 + 光标禁用 + **说明原因**（tooltip/helper） |
| loading | 按钮 busy + spinner；保持原宽度防跳动 |
| error | 行内红字 + 图标；字段聚焦 |
| selected | 明确底色/勾选，不只靠颜色 |

## 六、导航

- 底部 Tab：3–5 个，图标 + 文字，当前项高亮；不把“更多”塞进 Tab。
- 平板/桌面：≥600–840dp 用左侧 Rail；手机用底部 Tab（[Android responsive navigation](https://developer.android.google.cn/develop/ui/views/layout/build-responsive-navigation?hl=en)，仅读摘要）。
- 头部在页面转场时固定，不跟着滑（[Next.js view transitions](https://nextjs.org/docs/app/guides/view-transitions.md)）。
- 返回层级明确；根页面禁用“上一级”。

## 七、反馈

| 场景 | 组件 | 规则 |
|---|---|---|
| 轻量成功 | toast | 2.6s，`role=status`，可点关闭 |
| 错误 | toast/行内 | `role=alert`，不自动消失；文案说清“怎么修” |
| 页面级失败 | banner | 常驻 + 重试，不用 toast |
| 表单错误 | 字段下 | 失焦校验；`aria-describedby`；首次无效字段聚焦 |
| 破坏性 | 确认条/模态 | 主动作写明对象（“删除设备”），危险色 |
| 需要阅读 | 模态 | 焦点陷阱、Esc、背景 inert、关闭归还焦点 |
| 未知等待 | 骨架/spinner | 禁止假进度 |

## 八、空态 / 加载 / 错误

- 空态五要素：图标、邀请式标题、价值副标题、唯一主 CTA、可选次路径（[Appy Pie 空态](https://www.appypie.com/blog/app-empty-states)）。
- 骨架→内容：骨架淡出 120ms，内容淡入 + 上移 6px/180ms；无数据继续骨架。
- 错误：说清 what/why/how，不暴露堆栈/HTTP 码（[Microcopy Reference](https://raw.githubusercontent.com/danilonovaisv/portfolio-danilo-final/main/.claude/plugins/ghost-design-system/skills/claude-design-auditor/references/microcopy.md)）。

## 九、动效与编排

- 100–300ms；进入 ease-out、退出 ease-in；只动 transform/opacity（[Motion 参考](https://raw.githubusercontent.com/curiositech/windags-skills/main/skills/beautiful-gui-design/references/04-motion-and-microinteractions.md)）。
- 列表 stagger 每项 40–60ms，**超过 5 项后同延迟**，总时长 ≤300ms（[Choreography Recipes](https://raw.githubusercontent.com/emilkowalski/skills/main/skills/choreograph/RECIPES.md)）。
- 页面转场用 View Transitions API；退出 150ms、进入 210ms；头部固定；`pointer-events:none`；reduced-motion 全关（[Next.js](https://nextjs.org/docs/app/guides/view-transitions.md)）。
- `will-change` 只在动画前临时加，结束删除；不要给 body 加（[MDN](https://developer.mozilla.org/en-US/docs/Web/CSS/Reference/Properties/will-change)）。
- 主题切换：从按钮圆形扩散 320ms；或不支持时 280ms 颜色过渡；只过渡颜色相关属性。

## 十、文案（中文）

- 按钮写动词 + 对象：“保存设置”“删除这台设备”，不写“确定/OK”。
- 错误三步：发生了什么、为什么、怎么修；不怪用户、不露技术细节。
- 术语表固定：设备/场景/事件/插件/内核设置/隐私等级；不同页面不混用“用户/成员/账号”。
- 成功提示点名对象：“插件已导入”“密码已修改”。
- 占位符只给格式示例，不替代 label（Microcopy Reference）。

## 十一、无障碍

- 触控 ≥24（WCAG 2.2，AA），应用内 ≥44。
- 焦点环 2px、对比 ≥3:1（[WCAG 2.4.13 释义](https://www.digitalpolicy.gov.hk/en/our_work/digital_infrastructure/digital_inclusion/accessibility/promulgating_resources/handbook/appendix_a/a21_2413_focus_appearance.html)）。
- toast 的 role、模态的 dialog/aria-modal/inert（前文已落地）。
- 图标按钮必须有 `aria-label`；纯颜色不用作唯一状态。
- 支持 `prefers-reduced-motion`；保留功能、去掉位移。
- 键盘：Tab 顺序、Esc 关闭、焦点归还；终端快捷键不与浏览器冲突（Ctrl+A 需在非输入态拦截）。

## 十二、性能（感知流畅）

- INP 目标 ≤200ms（[web.dev 优化 INP](https://web.developers.google.cn/articles/optimize-inp?hl=zh-cn)；页面正文未读完，目标值来自 Core Web Vitals 通用标准）。
- 避免长任务（>50ms）；大列表分批渲染/`content-visibility:auto`。
- 减少整页 innerHTML 重绘；列表用 key 复用。
- CLS <0.05：骨架尺寸与内容一致；图片/卡片预留高度。
- 转场只用合成层；`will-change` 临时用。

## 十三、移动 / PWA

- `viewport-fit=cover`；底部 safe-area；软键盘弹出时输入框不被 Tab 遮挡。
- PWA 离线显示缓存/上次数据时必须标注“离线/可能过期”，不能冒充实时。
- 安装引导不重复弹；提供“稍后”。

## 十四、LING OS 验收清单（本轮）

- [ ] 8pt 间距审计：设置、设备、文件、系统四页
- [ ] 对比审计：日/夜，正文 4.5:1、大字 3:1、占位符 3:1
- [ ] 触控：所有按钮/行 ≥44；图标钮有 aria-label
- [ ] 组件 8 态：按钮、输入、开关、Tab、卡片
- [ ] 空态五要素：设备、文件、插件、事件
- [ ] 转场：前进/后退方向、头部固定、reduced-motion 降级
- [ ] 骨架→内容交接；CLS <0.05
- [ ] 控制台只保留版本连点（3 秒、44px、自动滚到面板）
- [ ] 文案：按钮动词、错误三步、术语统一
- [ ] INP 采样 ≤200ms；转场 ≥50fps

## 十五、来源与限制

已读正文：Next.js view transitions、MDN will-change、WCAG 焦点外观释义、SSW toast、Motion 参考、Choreography Recipes、Microcopy Reference、B站两条字幕（6 技巧、精度三感）、Apple HIG typography JSON、Material tone 博文（仅标题/摘要）、Android responsive navigation（仅摘要）、web.dev INP（页面正文截断，仅用通用 200ms 目标）。
仅登记未读：Appy Pie 空态（上轮读过）、Component State Matrix（抓取超时，规则为通用实践）、部分 B站视频。
