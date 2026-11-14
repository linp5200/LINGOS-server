# LING OS 0.14 · 流畅度与过渡计划

> 2026-11-14 先生反馈：仍不够流畅；要求继续查 UI、先给计划；控制台不再放系统页按钮，恢复“关于页版本连点 3 次”进入（之前点了没反应）。
> 本文是检索后的计划，**未动正式版、未部署**。预览版在 `lingos-preview/`，数据一律 `PVnodata`。

## 一、实际检索到的依据

| 来源 | 结论 | 采用 |
|---|---|---|
| [Designing view transitions（Next.js 官方指南）](https://nextjs.org/docs/app/guides/view-transitions.md) | 同文档转场四模式：共享元素 morph、骨架→内容、方向性前进/后退、同路由交叉淡入；退出 150ms、进入 210ms、位移 400ms；头部固定；转场层 `pointer-events:none`；`prefers-reduced-motion` 全关 | 页面切换用 View Transitions API + 旧浏览器回退；骨架交接、方向性、头部固定 |
| [MDN `will-change`](https://developer.mozilla.org/en-US/docs/Web/CSS/Reference/Properties/will-change) | 只在动画前临时加、动画后移除；不要给 `body` 大面积加；滥用反而更慢 | 仅对终端/模态/正在切换的单屏临时加，结束后删 |
| [Motion & Micro-Interactions](https://raw.githubusercontent.com/curiositech/windags-skills/main/skills/beautiful-gui-design/references/04-motion-and-microinteractions.md)（上轮） | 100–300ms，只动 transform/opacity；退出快、进入慢；同时 ≤3 个 | token 继续沿用；新增转场时序 |
| [WCAG 2.2 焦点外观](https://www.digitalpolicy.gov.hk/en/our_work/digital_infrastructure/digital_inclusion/accessibility/promulgating_resources/handbook/appendix_a/a21_2413_focus_appearance.html) | 焦点环 ≥2px、对比 ≥3:1 | 转场中保持焦点可见 |
| [B站《6 个 UI 设计技巧》](https://www.bilibili.com/video/BV1cKpee7E2a)（7:41，字幕已读） | ① 中性灰决定骨架，灰度只用两端、少用中间；② 正文 14–16，对比 ≥4.5:1（大字 3:1）；③ 8pt 间距网格；④ 用熟悉组件，不发明图标；⑤ 动画要克制，先保证质量和目的 | 做一次间距/灰度/对比审计；不为“流畅”加装饰动画 |
| [Android Slow rendering](https://developer.android.google.cn/topic/performance/issues/render) | 每帧 16.6ms；掉帧来自主线程布局/绘制 | 转场只用合成层；避免整页 innerHTML 反复替换 |

## 二、现在为什么不流畅（现状判断）

1. **路由切换是整页 `innerHTML` 替换**：旧内容瞬间消失、新内容瞬间出现，中间没有交接，所以“跳”。
2. **骨架→内容没有过渡**：骨架直接消失，数据一到内容“弹”出来。
3. **主题切换只做了颜色过渡，没有方向/来源**：先生上轮要的“从按钮扩散”还没做；夜间/日间大面积变色仍有闪。
4. **点击无即时反馈**：卡片/按钮按下没有 90ms 内的视觉回应，触屏上像没点到。
5. **列表一次性全部入场**：事件、内核 61 项、文件列表同时动，主线程压力大。
6. **控制台入口混乱**：系统页按钮 + Ctrl+A + Ctrl+` 三套入口；版本连点窗口只有 1.5s，手机稍慢就失败，且面板在下方、没自动滚到，点了像没反应。

## 三、计划

### P0 流畅度（先做这 6 条）

1. **页面转场（View Transitions API，失败回退）**
   - `document.startViewTransition(() => render())`，支持就用，不支持走现有 `.is-enter`。
   - 前进/后退按 tab 顺序给方向：前进新页从右 8px、旧页向左 8px；后退反向；同路由换分类用交叉淡入。
   - 头部 bar、tabbar 固定不动（命名后 `animation:none`）。
   - 退出 150ms、进入 210ms、位移 240ms；`::view-transition{pointer-events:none}`。
2. **主题切换从按钮扩散**
   - 在按钮位置记录圆心，用 View Transition 的圆形 `clip-path` 揭示 320ms；不支持则退回 280ms 颜色过渡（已有）。
   - 同时只过渡颜色相关属性，不用 `*` 长驻。
3. **骨架→内容交接**
   - 骨架淡出 120ms、内容淡入+上移 6px 180ms；数据未回来时保持骨架，不显示假数据。
4. **点击反馈**
   - 全局 `button/.dev-card/.adapter:active { transform: scale(.985); transition: transform 90ms }`；`touch-action: manipulation` 去双击缩放延迟。
5. **列表节制**
   - 首屏只给前 5 项 stagger（每项 40ms），其余直接显示；`content-visibility:auto` 用于超过 40 项的内核/事件列表。
6. **控制台入口收口**
   - 移除系统页「控制台」按钮和界面提示；只保留关于页版本连点 3 次进入。
   - 连点窗口改 **3 秒**；版本行整行可点（44px 高），每点一次给轻微按压反馈；第 3 次后自动 `scrollIntoView` 到测试工作台面板。
   - 进入后终端入口仍在工作台面板里（“打开终端”），快捷键 `Ctrl+A`/`Ctrl+Q` 保留但不作为唯一入口、不在界面提示。
   - 若用户点 1–2 次，不弹 toast 打扰；只在成功进入时给 info toast。

### P1 视觉/一致性（同批做）

- 一次间距审计：统一 4/8/12/16/24 阶梯（B站 8pt 原则），改掉非标 gap。
- 一次灰度/对比审计：正文 ≥4.5:1，夜间同；把过饱和色收敛到 moss + ember 两个强调色。
- 图标按钮统一 44px 触控；列表行整行可点，不靠小箭头。
- 去掉重复 h2、重复账号行（已做，回归检查）。

### P2 测量与回归

- Playwright 采样：页面切换期间 `requestAnimationFrame` 记录掉帧；目标 ≥50fps（60 为佳）。
- 记录 `PerformanceObserver('layout-shift')`：CLS <0.05。
- 关掉动效（prefers-reduced-motion / 偏好=关闭）时，转场必须 0ms 完成且功能不丢。
- 390×667/844、1440×900 三尺寸回归；终端打开/关闭时布局不抖。

## 四、验收

| 项 | 标准 |
|---|---|
| 切换 | 七个视图前进/后退有方向交接；头部与 Tab 不跟着动 |
| 主题 | 从按钮圆形扩散；不支持时颜色平滑过渡；无大面积闪白 |
| 骨架 | 骨架→内容有淡出淡入，不“弹” |
| 按压 | 按钮/卡片按下 90ms 内有缩放反馈 |
| 列表 | 首屏 stagger ≤5 项；长列表滚动不掉帧 |
| 控制台 | 系统页无控制台按钮；关于页版本连点 3 次（3 秒内）必进；面板自动可见 |
| 降级 | reduced-motion/动效关闭时无动画但功能完整 |
| 数据 | 预览版全部 `PVnodata`；正式版无假数据 |

## 五、风险

- View Transitions 在旧 WebView 不支持：必须检测 `document.startViewTransition`，否则走旧路径，不能报错。
- 转场期间点击会被覆盖层吃掉：加 `pointer-events:none`，转场 ≤260ms。
- 圆形扩散在大屏上可能覆盖过久：上限 320ms，并尊重 reduced-motion。
- 版本连点是隐藏入口：不加可见提示，但保留 44px 命中区和按压反馈。

## 六、下一步

1. 先生批准本计划后，先在 `lingos-preview/` 做可点的转场/主题扩散/连点演示，供批注。
2. 批注确认后同步到 `webdesk/`，跑 Playwright + 帧率/CLS 采样。
3. 两个独立审查（不告知门槛）；达到我设定的门槛才 bump 版本、构建、装手机。
