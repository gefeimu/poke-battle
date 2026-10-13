/*
 * 绘制原语实现。接口与设计理由见 poke_render.h。
 */
#include "poke_render.h"

#include <stddef.h>

/* ---------------------------------------------------------------- 颜色 */

/*
 * 单通道混合 + 四舍五入。
 *
 * 注意负数要单独处理：C 的整数除法是**向零截断**，直接写 (num + 127) / 255
 * 在 num 为负时会少 1，导致往暗处混合时偏亮一个台阶。
 */
static inline int blend_ch(int d, int s, int alpha)
{
    int num = (s - d) * alpha;
    int delta = (num >= 0) ? (num + 127) / 255 : -((-num + 127) / 255);
    return d + delta;
}

uint16_t r_blend(uint16_t dst, uint16_t src, uint8_t alpha)
{
    if (alpha == 0) {
        return dst;
    }
    if (alpha >= 255) {
        return src;
    }

    int r = blend_ch((dst >> 11) & 0x1F, (src >> 11) & 0x1F, alpha);
    int g = blend_ch((dst >> 5) & 0x3F, (src >> 5) & 0x3F, alpha);
    int b = blend_ch(dst & 0x1F, src & 0x1F, alpha);

    return (uint16_t)((r << 11) | (g << 5) | b);
}

/* ---------------------------------------------------------------- 缓冲 */

static void sync_default_clip(Surface *s)
{
    /* 默认裁剪 = 整块缓冲在屏幕上占的范围 */
    s->cx0 = s->ox;
    s->cy0 = s->oy;
    s->cx1 = (int16_t)(s->ox + s->bw);
    s->cy1 = (int16_t)(s->oy + s->bh);
}

void r_init(Surface *s, uint16_t *buf, int buf_w, int buf_h, int stride)
{
    s->pixels = buf;
    s->bw = (int16_t)buf_w;
    s->bh = (int16_t)buf_h;
    s->stride = (int16_t)(stride > 0 ? stride : buf_w);
    s->ox = 0;
    s->oy = 0;
    sync_default_clip(s);
}

void r_set_origin(Surface *s, int ox, int oy)
{
    s->ox = (int16_t)ox;
    s->oy = (int16_t)oy;
    sync_default_clip(s);
}

void r_set_clip(Surface *s, int x, int y, int w, int h)
{
    if (w <= 0 || h <= 0) {
        r_reset_clip(s);
        return;
    }
    sync_default_clip(s);

    int x0 = x, y0 = y, x1 = x + w, y1 = y + h;
    if (x0 < s->cx0) x0 = s->cx0;
    if (y0 < s->cy0) y0 = s->cy0;
    if (x1 > s->cx1) x1 = s->cx1;
    if (y1 > s->cy1) y1 = s->cy1;
    if (x1 < x0) x1 = x0;
    if (y1 < y0) y1 = y0;

    s->cx0 = (int16_t)x0;
    s->cy0 = (int16_t)y0;
    s->cx1 = (int16_t)x1;
    s->cy1 = (int16_t)y1;
}

void r_reset_clip(Surface *s)
{
    sync_default_clip(s);
}

/* ---------------------------------------------------------------- 内部 */

bool r_inside(const Surface *s, int x, int y)
{
    return x >= s->cx0 && x < s->cx1 && y >= s->cy0 && y < s->cy1
        && x >= s->ox && y >= s->oy
        && (x - s->ox) < s->bw && (y - s->oy) < s->bh;
}

static inline void put(Surface *s, int x, int y, uint16_t c)
{
    if (!r_inside(s, x, y)) {
        return;
    }
    s->pixels[(y - s->oy) * s->stride + (x - s->ox)] = c;
}

static inline uint16_t get(const Surface *s, int x, int y)
{
    return s->pixels[(y - s->oy) * s->stride + (x - s->ox)];
}

/*
 * 圆角矩形内部判定。
 *
 * 做法是把点分别向 x / y 方向 clamp 到一个「去掉圆角后的内矩形」上，
 * 得到最近的中心点，再比距离。点落在中间带时 clamp 后就是它自己，
 * 距离为 0 —— 那正是「中间区域恒为实心」的体现。
 */
static bool in_rrect(int px, int py, int x0, int y0, int x1, int y1, int r)
{
    if (px < x0 || px > x1 || py < y0 || py > y1) {
        return false;
    }
    if (r <= 0) {
        return true;
    }
    int cx = px;
    int cy = py;
    if (px < x0 + r) {
        cx = x0 + r;
    } else if (px > x1 - r) {
        cx = x1 - r;
    }
    if (py < y0 + r) {
        cy = y0 + r;
    } else if (py > y1 - r) {
        cy = y1 - r;
    }
    int dx = px - cx;
    int dy = py - cy;
    return dx * dx + dy * dy <= r * r;
}

/* ---------------------------------------------------------------- 图元 */

void r_pixel(Surface *s, int x, int y, uint16_t c)
{
    put(s, x, y, c);
}

void r_fill(Surface *s, uint16_t c)
{
    for (int y = s->cy0; y < s->cy1; y++) {
        for (int x = s->cx0; x < s->cx1; x++) {
            put(s, x, y, c);
        }
    }
}

void r_fill_rect(Surface *s, int x, int y, int w, int h, uint16_t c)
{
    if (w <= 0 || h <= 0) {
        return;
    }
    for (int i = 0; i < h; i++) {
        for (int j = 0; j < w; j++) {
            put(s, x + j, y + i, c);
        }
    }
}

void r_hline(Surface *s, int x, int y, int w, uint16_t c)
{
    if (w <= 0) {
        return;
    }
    for (int j = 0; j < w; j++) {
        put(s, x + j, y, c);
    }
}

void r_vline(Surface *s, int x, int y, int h, uint16_t c)
{
    if (h <= 0) {
        return;
    }
    for (int i = 0; i < h; i++) {
        put(s, x, y + i, c);
    }
}

void r_round_rect(Surface *s, int x, int y, int w, int h, int radius, uint16_t c)
{
    if (w <= 0 || h <= 0) {
        return;
    }
    if (radius < 0) {
        radius = 0;
    }
    if (radius > w / 2) {
        radius = w / 2;
    }
    if (radius > h / 2) {
        radius = h / 2;
    }

    /* 先与裁剪求交，避免对着整块缓冲逐像素空转 */
    int x0 = x, y0 = y, x1 = x + w - 1, y1 = y + h - 1;
    if (x0 < s->cx0) x0 = s->cx0;
    if (y0 < s->cy0) y0 = s->cy0;
    if (x1 >= s->cx1) x1 = s->cx1 - 1;
    if (y1 >= s->cy1) y1 = s->cy1 - 1;
    if (x1 < x0 || y1 < y0) {
        return;
    }

    for (int py = y0; py <= y1; py++) {
        for (int px = x0; px <= x1; px++) {
            if (in_rrect(px, py, x, y, x + w - 1, y + h - 1, radius)) {
                put(s, px, py, c);
            }
        }
    }
}

void r_round_rect_outline(Surface *s, int x, int y, int w, int h, int radius,
                          uint16_t edge, int thickness, uint32_t fill)
{
    if (w <= 0 || h <= 0) {
        return;
    }
    if (thickness < 1) {
        thickness = 1;
    }

    if (fill != R_NO_FILL) {
        /* 有填充：先铺满描边色，再用填充色盖住内部。
         * 两步都是普通的实心圆角矩形，最省事。 */
        r_round_rect(s, x, y, w, h, radius, edge);
        int iw = w - 2 * thickness;
        int ih = h - 2 * thickness;
        if (iw <= 0 || ih <= 0) {
            return;
        }
        int ir = radius - thickness;
        if (ir < 0) {
            ir = 0;
        }
        r_round_rect(s, x + thickness, y + thickness, iw, ih, ir, (uint16_t)fill);
        return;
    }

    /*
     * 只描边：必须逐像素判断「在外框内但不在内框内」，也就是画一个**环**。
     *
     * 不能走上面那条路 —— 上面那种「先铺满再挖空」的写法在 fill == R_NO_FILL
     * 时会退化成「把整个矩形铺成描边色」，内部原有的内容全被覆盖。
     * 对话框的内框线就是这么把白底刷成蓝底的（画出来才发现）。
     */
    int ir = radius - thickness;
    if (ir < 0) {
        ir = 0;
    }
    const int ix = x + thickness;
    const int iy = y + thickness;
    const int iw = w - 2 * thickness;
    const int ih = h - 2 * thickness;

    int x0 = x, y0 = y, x1 = x + w - 1, y1 = y + h - 1;
    for (int py = y0; py <= y1; py++) {
        for (int px = x0; px <= x1; px++) {
            if (!in_rrect(px, py, x0, y0, x1, y1, radius)) {
                continue;
            }
            if (iw > 0 && ih > 0
                && in_rrect(px, py, ix, iy, ix + iw - 1, iy + ih - 1, ir)) {
                continue;   /* 在内部，不属于环 */
            }
            put(s, px, py, edge);
        }
    }
}

void r_ellipse(Surface *s, int cx, int cy, int rx, int ry, uint16_t c)
{
    if (rx <= 0 || ry <= 0) {
        return;
    }

    /* 整数判定：dx²/b² + dy²/a² <= 1  等价于  dx²*a + dy²*b <= a*b */
    const long long a = (long long)rx * rx;
    const long long b = (long long)ry * ry;

    for (int dy = -ry; dy <= ry; dy++) {
        for (int dx = -rx; dx <= rx; dx++) {
            if ((long long)dx * dx * b + (long long)dy * dy * a <= a * b) {
                put(s, cx + dx, cy + dy, c);
            }
        }
    }
}

void r_grad_v(Surface *s, int x, int y, int w, int h,
              uint16_t top, uint16_t bot)
{
    if (w <= 0 || h <= 0) {
        return;
    }

    int tr = (top >> 11) & 0x1F, tg = (top >> 5) & 0x3F, tb = top & 0x1F;
    int br = (bot >> 11) & 0x1F, bg = (bot >> 5) & 0x3F, bb = bot & 0x1F;

    for (int i = 0; i < h; i++) {
        int t = (h > 1) ? (i * 255 / (h - 1)) : 0;
        /*
         * 用 blend_ch 而不是手写 (b - a) * t + 127) / 255：
         * 后者在**往暗处**插值（b < a）时因为 C 的除法向零截断会少减，
         * 实测地面渐变能偏亮 16 个色阶 —— 肉眼能看出来。
         */
        int r = blend_ch(tr, br, t);
        int g = blend_ch(tg, bg, t);
        int b = blend_ch(tb, bb, t);
        uint16_t c = (uint16_t)((r << 11) | (g << 5) | b);
        r_hline(s, x, y + i, w, c);
    }
}

void r_sprite(Surface *s, int x, int y, const SpriteEntry *spr)
{
    if (!spr || !spr->data || !spr->palette) {
        return;
    }

    for (int gy = 0; gy < spr->h; gy++) {
        for (int gx = 0; gx < spr->w; gx++) {
            /*
             * 4bpp 索引色：每字节 2 像素，高位在前。
             * 索引 0 是透明，不绘制 —— 精灵图没有 alpha 通道，
             * 透明就是「这个像素不存在」。
             */
            int i = gy * spr->w + gx;
            uint8_t idx = (uint8_t)((spr->data[i / 2] >> ((i % 2) ? 0 : 4)) & 0x0F);
            if (idx == PAL_TRANSPARENT) {
                continue;
            }
            put(s, x + gx, y + gy, spr->palette[idx]);
        }
    }
}

/* ---------------------------------------------------------------- 文字 */

int r_text(Surface *s, int x, int y, const BitmapFont *f,
           const char *text, uint16_t color)
{
    if (!s || !f || !text) {
        return x;
    }

    /*
     * y 是行框左上角，基线在下面 base_line 处（往上数）。
     * 于是字形墨水的底边 = 基线 - ofs_y，顶边再往上 box_h。
     */
    const int baseline = y + (f->line_height - f->base_line);
    const int bpp = f->bpp;
    const uint32_t maxv = (1u << bpp) - 1u;

    /* 用 1/16 像素单位累加（与 adv_w 同单位），最后再取整 */
    int pen = x << 4;

    const char *p = text;
    while (*p) {
        uint32_t cp = font_utf8_next(&p);
        uint16_t gi = font_glyph_index(f, cp);
        if (gi == FONT_GLYPH_NONE) {
            continue;   /* 缺字：不绘制也不占宽度 */
        }
        const GlyphDsc *g = font_glyph(f, gi);
        if (!g) {
            continue;
        }

        const int gx0 = (pen >> 4) + g->ofs_x;
        const int gy0 = baseline - g->ofs_y - g->box_h;

        for (int row = 0; row < g->box_h; row++) {
            for (int col = 0; col < g->box_w; col++) {
                /*
                 * 连续位打包，MSB 在前。bpp 取 1/2/4/8 时单个像素
                 * 不会跨字节，所以取字节内偏移即可，不必拼接。
                 */
                uint32_t k = (uint32_t)row * g->box_w + (uint32_t)col;
                uint32_t bit = k * (uint32_t)bpp;
                uint8_t byte = f->bitmap[g->bitmap_index + (bit >> 3)];
                uint8_t v = (uint8_t)((byte >> (8 - bpp - (int)(bit & 7))) & maxv);
                if (v == 0) {
                    continue;
                }

                int px = gx0 + col;
                int py = gy0 + row;
                if (!r_inside(s, px, py)) {
                    continue;
                }

                /* 灰度值 -> alpha：4bpp 时 0..15 映射到 0..255 */
                uint8_t a = (uint8_t)((v * 255u + maxv - 1u) / maxv);
                put(s, px, py, r_blend(get(s, px, py), color, a));
            }
        }

        pen += g->adv_w;
    }

    return (pen + 8) >> 4;
}

int r_text_outlined(Surface *s, int x, int y, const BitmapFont *f,
                    const char *text, uint16_t fill, uint16_t edge)
{
    /* 四向各画一遍描边色，再画本体。小字号下对可读性提升很明显。 */
    r_text(s, x - 1, y, f, text, edge);
    r_text(s, x + 1, y, f, text, edge);
    r_text(s, x, y - 1, f, text, edge);
    r_text(s, x, y + 1, f, text, edge);
    return r_text(s, x, y, f, text, fill);
}

int r_text_width(const BitmapFont *f, const char *text)
{
    return font_text_width(f, text);
}
