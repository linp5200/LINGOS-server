#ifndef COMMON_MARKDOWN_RENDERER_H
#define COMMON_MARKDOWN_RENDERER_H

/* ============================================================
 * Markdown → ANSI 富文本渲染（终端）
 * 支持：**粗体** *斜体* `代码` | 表格 | --- 分隔线 - 列表 ```代码块```
 * 链接/图片不渲染（终端环境）
 * ============================================================ */

/**
 * @brief 渲染单行 markdown（内联样式 + 行类型识别）
 * @param line 单行文本（不含换行）
 */
void md_render_line(const char *line);

/**
 * @brief 渲染多行 markdown 文本（含代码块状态跟踪）
 * @param text 多行文本
 */
void md_render_text(const char *text);

/**
 * @brief 流式渲染内容块（内联粗体/斜体/代码，容忍跨块截断）
 * @param delta 流式内容块
 */
void md_render_stream_delta(const char *delta);

/**
 * @brief 流式逐字符喂入（累积到换行后按行渲染：表格/列表/分隔线/内联）
 * @param text 流式文本块
 */
void md_stream_feed(const char *text);

/**
 * @brief 刷新剩余缓冲（流式结束调用）
 */
void md_stream_flush(void);

/**
 * @brief 判断字符串是否为表格行（含 | 分隔）
 */
int md_is_table_row(const char *line);

#endif /* COMMON_MARKDOWN_RENDERER_H */
