/**
 * @file    src/monitor/osd_overlay.c
 * @brief   OSD 叠加实现（5x7 点阵真实绘制 + 时间戳/摄像头 ID）
 * @version LN-0.9.0
 * @par     核心协议：诚实红线（不绘制 = 明确报告，绝不用假像素冒充）
 *
 * 像素格式说明（真实的字节布局，非近似）：
 *   YUYV(YUY2) 每像素 2 字节：[Y0 U Y1 V]；像素 x 的亮度字节偏移 = (y*width + x) * 2
 *   写入亮度同时把同组 U/V 置 128（中性灰）——避免叠加区出现彩色镶边
 *   JPEG/MJPEG：需解码后才能绘制，本模块不做（返回 OSD_ERR_FORMAT 并如实记录）
 */

#include "osd_overlay.h"
#include "../common/safe_string.h"
#include "../common/lang.h"
#include "../lib/log_extra.h"
#include <stdio.h>
#include <string.h>
#include <time.h>

/* ============================================================
 * 内置 5x7 点阵字体（ASCII 0x20–0x5F，共 64 字形 × 5 列）
 * 列优先：每字节 bit0..bit6 = 自上而下 7 行（经典 5x7 点阵编码）
 * ============================================================ */

#define OSD_FONT_FIRST 0x20
#define OSD_FONT_LAST  0x5F

static const unsigned char g_font5x7[(OSD_FONT_LAST - OSD_FONT_FIRST + 1) * 5] = {
    0x00, 0x00, 0x00, 0x00, 0x00,  /* ' ' */
    0x00, 0x00, 0x5F, 0x00, 0x00,  /* '!' */
    0x00, 0x07, 0x00, 0x07, 0x00,  /* '"' */
    0x14, 0x7F, 0x14, 0x7F, 0x14,  /* '#' */
    0x24, 0x2A, 0x7F, 0x2A, 0x12,  /* '$' */
    0x23, 0x13, 0x08, 0x64, 0x62,  /* '%' */
    0x36, 0x49, 0x55, 0x22, 0x50,  /* '&' */
    0x00, 0x05, 0x03, 0x00, 0x00,  /* '\'' */
    0x00, 0x1C, 0x22, 0x41, 0x00,  /* '(' */
    0x00, 0x41, 0x22, 0x1C, 0x00,  /* ')' */
    0x14, 0x08, 0x3E, 0x08, 0x14,  /* '*' */
    0x08, 0x08, 0x3E, 0x08, 0x08,  /* '+' */
    0x00, 0x50, 0x30, 0x00, 0x00,  /* ',' */
    0x08, 0x08, 0x08, 0x08, 0x08,  /* '-' */
    0x00, 0x60, 0x60, 0x00, 0x00,  /* '.' */
    0x20, 0x10, 0x08, 0x04, 0x02,  /* '/' */
    0x3E, 0x51, 0x49, 0x45, 0x3E,  /* '0' */
    0x00, 0x42, 0x7F, 0x40, 0x00,  /* '1' */
    0x42, 0x61, 0x51, 0x49, 0x46,  /* '2' */
    0x21, 0x41, 0x45, 0x4B, 0x31,  /* '3' */
    0x18, 0x14, 0x12, 0x7F, 0x10,  /* '4' */
    0x27, 0x45, 0x45, 0x45, 0x39,  /* '5' */
    0x3C, 0x4A, 0x49, 0x49, 0x30,  /* '6' */
    0x01, 0x71, 0x09, 0x05, 0x03,  /* '7' */
    0x36, 0x49, 0x49, 0x49, 0x36,  /* '8' */
    0x06, 0x49, 0x49, 0x29, 0x1E,  /* '9' */
    0x00, 0x36, 0x36, 0x00, 0x00,  /* ':' */
    0x00, 0x56, 0x36, 0x00, 0x00,  /* ';' */
    0x08, 0x14, 0x22, 0x41, 0x00,  /* '<' */
    0x14, 0x14, 0x14, 0x14, 0x14,  /* '=' */
    0x00, 0x41, 0x22, 0x14, 0x08,  /* '>' */
    0x02, 0x01, 0x51, 0x09, 0x06,  /* '?' */
    0x32, 0x49, 0x79, 0x41, 0x3E,  /* '@' */
    0x7E, 0x11, 0x11, 0x11, 0x7E,  /* 'A' */
    0x7F, 0x49, 0x49, 0x49, 0x36,  /* 'B' */
    0x3E, 0x41, 0x41, 0x41, 0x22,  /* 'C' */
    0x7F, 0x41, 0x41, 0x22, 0x1C,  /* 'D' */
    0x7F, 0x49, 0x49, 0x49, 0x41,  /* 'E' */
    0x7F, 0x09, 0x09, 0x09, 0x01,  /* 'F' */
    0x3E, 0x41, 0x49, 0x49, 0x7A,  /* 'G' */
    0x7F, 0x08, 0x08, 0x08, 0x7F,  /* 'H' */
    0x00, 0x41, 0x7F, 0x41, 0x00,  /* 'I' */
    0x20, 0x40, 0x41, 0x3F, 0x01,  /* 'J' */
    0x7F, 0x08, 0x14, 0x22, 0x41,  /* 'K' */
    0x7F, 0x40, 0x40, 0x40, 0x40,  /* 'L' */
    0x7F, 0x02, 0x0C, 0x02, 0x7F,  /* 'M' */
    0x7F, 0x04, 0x08, 0x10, 0x7F,  /* 'N' */
    0x3E, 0x41, 0x41, 0x41, 0x3E,  /* 'O' */
    0x7F, 0x09, 0x09, 0x09, 0x06,  /* 'P' */
    0x3E, 0x41, 0x51, 0x21, 0x5E,  /* 'Q' */
    0x7F, 0x09, 0x19, 0x29, 0x46,  /* 'R' */
    0x46, 0x49, 0x49, 0x49, 0x31,  /* 'S' */
    0x01, 0x01, 0x7F, 0x01, 0x01,  /* 'T' */
    0x3F, 0x40, 0x40, 0x40, 0x3F,  /* 'U' */
    0x1F, 0x20, 0x40, 0x20, 0x1F,  /* 'V' */
    0x3F, 0x40, 0x38, 0x40, 0x3F,  /* 'W' */
    0x63, 0x14, 0x08, 0x14, 0x63,  /* 'X' */
    0x07, 0x08, 0x70, 0x08, 0x07,  /* 'Y' */
    0x61, 0x51, 0x49, 0x45, 0x43,  /* 'Z' */
    0x00, 0x7F, 0x41, 0x41, 0x00,  /* '[' */
    0x02, 0x04, 0x08, 0x10, 0x20,  /* '\\' */
    0x00, 0x41, 0x41, 0x7F, 0x00,  /* ']' */
    0x04, 0x02, 0x01, 0x02, 0x04,  /* '^' */
    0x40, 0x40, 0x40, 0x40, 0x40   /* '_' */
};

#define OSD_GLYPH_W 5
#define OSD_GLYPH_H 7
#define OSD_ADVANCE 6      /* 字距 1 像素 */

/* 亮度取值（YUYV Y 平面，BT.601 视频范围） */
#define OSD_LUMA_BG   16   /* 底框（黑） */
#define OSD_LUMA_FG  235   /* 文字（白） */
#define OSD_CHROMA   128   /* 中性色度 */

/* ============================================================
 * 模块状态
 * ============================================================ */

static int  g_enabled = 0;
static int  g_scale = 1;
static char g_camera_id[32] = {0};
static char g_status[224] = "OSD: not initialized";
static unsigned long g_format_skips = 0;   /* JPEG 跳过计数（限流日志用） */

/* ============================================================
 * 像素写入（YUYV）
 * ============================================================ */

static void yuyv_set_pixel(unsigned char *buf, int width, int height,
                           int x, int y, unsigned char luma) {
    if (x < 0 || y < 0 || x >= width || y >= height) return;
    size_t off = ((size_t)y * (size_t)width + (size_t)x) * 2u;
    buf[off] = luma;
    size_t pair = off & ~(size_t)3u;   /* [Y0 U Y1 V] 四字节组 */
    buf[pair + 1] = OSD_CHROMA;        /* U */
    buf[pair + 3] = OSD_CHROMA;        /* V */
}

static void yuyv_fill_rect(unsigned char *buf, int width, int height,
                           int x0, int y0, int w, int h, unsigned char luma) {
    for (int y = y0; y < y0 + h; y++) {
        for (int x = x0; x < x0 + w; x++) {
            yuyv_set_pixel(buf, width, height, x, y, luma);
        }
    }
}

/* ============================================================
 * 文本绘制
 * ============================================================ */

static void osd_draw_char(unsigned char *buf, int width, int height,
                          int x, int y, char ch, int scale, unsigned char luma) {
    unsigned char c = (unsigned char)ch;
    if (c < OSD_FONT_FIRST || c > OSD_FONT_LAST) c = (unsigned char)'?';
    if (c == (unsigned char)' ') return;   /* 空格不落笔 */

    const unsigned char *glyph = &g_font5x7[(size_t)(c - OSD_FONT_FIRST) * OSD_GLYPH_W];
    for (int col = 0; col < OSD_GLYPH_W; col++) {
        unsigned char bits = glyph[col];
        for (int row = 0; row < OSD_GLYPH_H; row++) {
            if (!(bits & (unsigned char)(1u << row))) continue;
            for (int sy = 0; sy < scale; sy++) {
                for (int sx = 0; sx < scale; sx++) {
                    yuyv_set_pixel(buf, width, height,
                                   x + col * scale + sx, y + row * scale + sy, luma);
                }
            }
        }
    }
}

int osd_overlay_draw_text_yuyv(unsigned char *buf, int width, int height,
                               int x, int y, const char *text, int scale) {
    if (!buf || !text || width <= 0 || height <= 0) return -1;
    if (scale <= 0) scale = 1;
    if (scale > 4) scale = 4;

    int drawn = 0;
    int cx = x;
    for (const char *p = text; *p; p++) {
        char ch = *p;
        if (ch >= 'a' && ch <= 'z') ch = (char)(ch - 'a' + 'A');   /* 字体仅含大写 */
        osd_draw_char(buf, width, height, cx, y, ch, scale, OSD_LUMA_FG);
        cx += OSD_ADVANCE * scale;
        drawn++;
    }
    return drawn;
}

/* ============================================================
 * 对外接口
 * ============================================================ */

int osd_overlay_init(int enable, const char *camera_id, int scale) {
    g_enabled = enable ? 1 : 0;
    g_scale = (scale > 0) ? (scale > 4 ? 4 : scale) : 1;
    safe_strncpy(g_camera_id, camera_id ? camera_id : "", sizeof(g_camera_id));
    safe_snprintf(g_status, sizeof(g_status),
                  "OSD: %s (scale=%d, camera=%s, font=builtin 5x7 ASCII 0x20-0x5F)",
                  g_enabled ? "enabled" : "disabled", g_scale,
                  g_camera_id[0] ? g_camera_id : "cam0");
    LOG_INFO_T("Osd", "Init", "OK", "%s", g_status);
    return 0;
}

int osd_overlay_apply(lingos_frame_t *frame) {
    if (!frame || !frame->data || frame->width <= 0 || frame->height <= 0 || frame->size <= 0) {
        safe_strncpy(g_status, "OSD: invalid frame (no data/size) — not drawn", sizeof(g_status));
        return OSD_ERR_INVALID;
    }

    if (!g_enabled) {
        safe_strncpy(g_status, "OSD: disabled by config — not drawn (honest skip)", sizeof(g_status));
        return OSD_SKIP_DISABLED;
    }

    if (frame->format != FRAME_FMT_YUYV) {
        /* JPEG/MJPEG：绘制需先解码——本模块不做（不伪造） */
        g_format_skips++;
        if (g_format_skips == 1 || (g_format_skips % 300) == 0) {
            LOG_WARN_T("Osd", "Apply", "FormatUnsupported",
                       "frame format=%s needs decode before OSD — skipped %lu frames "
                       "(honest: no burned-in timestamp for compressed frames)",
                       frame_format_name(frame->format), g_format_skips);
        }
        safe_snprintf(g_status, sizeof(g_status),
                      "OSD: format %s not drawable without decode — skipped (honest), skips=%lu",
                      frame_format_name(frame->format), g_format_skips);
        return OSD_ERR_FORMAT;
    }

    long need = (long)frame->width * (long)frame->height * 2L;
    if ((long)frame->size < need) {
        safe_snprintf(g_status, sizeof(g_status),
                      "OSD: YUYV buffer too small (size=%d < %ld) — not drawn",
                      frame->size, need);
        LOG_WARN_T("Osd", "Apply", "ShortBuffer", "%s", g_status);
        return OSD_ERR_INVALID;
    }

    /* 真实时间戳（帧自带时间优先，缺失则取当前时间） */
    time_t ts = frame->timestamp > 0 ? frame->timestamp : time(NULL);
    struct tm tmv;
    memset(&tmv, 0, sizeof(tmv));
    if (localtime_r(&ts, &tmv) == NULL) {
        safe_strncpy(g_status, "OSD: localtime_r failed — not drawn", sizeof(g_status));
        return OSD_ERR_INVALID;
    }
    char tsbuf[32];
    if (strftime(tsbuf, sizeof(tsbuf), "%Y-%m-%d %H:%M:%S", &tmv) == 0) {
        safe_strncpy(g_status, "OSD: strftime failed — not drawn", sizeof(g_status));
        return OSD_ERR_INVALID;
    }

    const char *cam = frame->camera_id[0] ? frame->camera_id
                    : (g_camera_id[0] ? g_camera_id : "cam0");
    char text[96];
    int tl = safe_snprintf(text, sizeof(text), "%s %s", tsbuf, cam);
    if (tl <= 0 || (size_t)tl >= sizeof(text)) {
        safe_strncpy(g_status, "OSD: text overflow — not drawn", sizeof(g_status));
        return OSD_ERR_INVALID;
    }

    /* 底框（提升可读性）+ 文本：真实逐像素写入 */
    const int pad = 3;
    const int x0 = 8;
    const int y0 = 8;
    int bw = tl * OSD_ADVANCE * g_scale + pad * 2;
    int bh = OSD_GLYPH_H * g_scale + pad * 2;
    if (bw > frame->width) bw = frame->width - x0;
    if (bh > frame->height) bh = frame->height - y0;
    if (bw <= 0 || bh <= 0) {
        safe_strncpy(g_status, "OSD: frame smaller than OSD box — not drawn", sizeof(g_status));
        return OSD_ERR_INVALID;
    }

    yuyv_fill_rect(frame->data, frame->width, frame->height, x0, y0, bw, bh, OSD_LUMA_BG);
    int drawn = osd_overlay_draw_text_yuyv(frame->data, frame->width, frame->height,
                                           x0 + pad, y0 + pad, text, g_scale);

    safe_snprintf(g_status, sizeof(g_status),
                  "OSD: drew \"%s\" (%d chars, %dx%d box) at (%d,%d) scale=%d",
                  text, drawn, bw, bh, x0, y0, g_scale);
    return OSD_OK;
}

int osd_overlay_enabled(void) {
    return g_enabled;
}

const char *osd_overlay_status(void) {
    return g_status;
}

void osd_overlay_cleanup(void) {
    g_enabled = 0;
    g_format_skips = 0;
    safe_strncpy(g_status, "OSD: cleaned up", sizeof(g_status));
}
