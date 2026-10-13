/*
 * 位图字体的查找与度量。数据结构见 poke_font.h。
 */
#include "poke_font.h"

#include <stddef.h>
#include <string.h>

uint16_t font_glyph_index(const BitmapFont *f, uint32_t cp)
{
    if (!f || !f->map || f->map_count == 0) {
        return FONT_GLYPH_NONE;
    }

    /* 二分查找。表按 codepoint 升序（生成时排好，test_render.c 断言）。 */
    uint32_t lo = 0;
    uint32_t hi = f->map_count;      /* 区间 [lo, hi) */
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        uint32_t c = f->map[mid].codepoint;
        if (c == cp) {
            return f->map[mid].glyph_index;
        }
        if (c < cp) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    return FONT_GLYPH_NONE;
}

const GlyphDsc *font_glyph(const BitmapFont *f, uint16_t index)
{
    /* 索引 0 是保留槽（LVGL 的 .notdef），没有可用字形 */
    if (!f || index == 0 || index >= f->glyph_count) {
        return NULL;
    }
    return &f->glyphs[index];
}

uint32_t font_utf8_next(const char **p)
{
    const unsigned char *s = (const unsigned char *)*p;

    if (s[0] < 0x80) {
        *p += 1;
        return s[0];
    }
    if ((s[0] & 0xE0) == 0xC0 && (s[1] & 0xC0) == 0x80) {
        *p += 2;
        return ((uint32_t)(s[0] & 0x1F) << 6) | (uint32_t)(s[1] & 0x3F);
    }
    if ((s[0] & 0xF0) == 0xE0 && (s[1] & 0xC0) == 0x80 && (s[2] & 0xC0) == 0x80) {
        *p += 3;
        return ((uint32_t)(s[0] & 0x0F) << 12)
             | ((uint32_t)(s[1] & 0x3F) << 6)
             |  (uint32_t)(s[2] & 0x3F);
    }
    if ((s[0] & 0xF8) == 0xF0 && (s[1] & 0xC0) == 0x80
        && (s[2] & 0xC0) == 0x80 && (s[3] & 0xC0) == 0x80) {
        *p += 4;
        return ((uint32_t)(s[0] & 0x07) << 18)
             | ((uint32_t)(s[1] & 0x3F) << 12)
             | ((uint32_t)(s[2] & 0x3F) << 6)
             |  (uint32_t)(s[3] & 0x3F);
    }

    /* 非法字节：当单字节处理，保证指针一定前进，不会死循环 */
    *p += 1;
    return s[0];
}

int font_text_width(const BitmapFont *f, const char *text)
{
    if (!f || !text) {
        return 0;
    }

    /*
     * 累加原始 1/16 单位，最后统一次取整。
     * 逐字取整会让长文本越走越窄（每字最多差 0.5px）。
     */
    uint32_t acc = 0;
    const char *p = text;
    while (*p) {
        uint32_t cp = font_utf8_next(&p);
        uint16_t gi = font_glyph_index(f, cp);
        if (gi == FONT_GLYPH_NONE) {
            continue;   /* 缺字不占宽度，与渲染时跳过一致 */
        }
        const GlyphDsc *g = font_glyph(f, gi);
        if (g) {
            acc += g->adv_w;
        }
    }
    return (int)((acc + 8) >> 4);
}

/* 单字的步进宽度，单位与 font_text_width 的累加器一致（1/16 像素）。
 * 缺字按 0 算 —— 与渲染时「查不到就跳过」保持一致。 */
static uint32_t glyph_adv(const BitmapFont *f, uint32_t cp)
{
    uint16_t gi = font_glyph_index(f, cp);
    if (gi == FONT_GLYPH_NONE) {
        return 0;
    }
    const GlyphDsc *g = font_glyph(f, gi);
    return g ? g->adv_w : 0;
}

int font_wrap2(const BitmapFont *f, const char *text, int max_w,
               char *l0, size_t cap0, char *l1, size_t cap1)
{
    if (l0 && cap0) {
        l0[0] = '\0';
    }
    if (l1 && cap1) {
        l1[0] = '\0';
    }
    if (!text || !*text || !l0 || cap0 < 2) {
        return 0;
    }

    int      line = 0;      /* 0 = 还在第一行；>=1 = 已在最后一行 */
    char    *dst  = l0;
    size_t   cap  = cap0;
    size_t   used = 0;
    uint32_t acc  = 0;      /* 本行宽度累加，1/16 像素 */

    const char *p = text;
    while (*p) {
        const char *ch_start = p;
        uint32_t cp = font_utf8_next(&p);
        size_t   nbytes = (size_t)(p - ch_start);

        if (cp == '\n') {   /* 引擎日志里没有换行，留着是为了通用 */
            if (line == 0) {
                line = 1;
                dst = l1;
                cap = cap1;
                used = 0;
                acc = 0;
            }
            continue;
        }

        uint32_t adv = glyph_adv(f, cp);

        /* 放不下就换行。注意判断里的 used > 0：**每行第一个字无条件放下**。 */
        if (used > 0 && (int)((acc + adv + 8) >> 4) > max_w) {
            if (line == 0) {
                line = 1;
                dst = l1;
                cap = cap1;
                used = 0;
                acc = 0;
            } else {
                line = 2;   /* 第三行起不再折，剩下的全塞进 l1 */
            }
        }

        if (dst == NULL || cap < 2 || used + nbytes + 1 > cap) {
            break;          /* 缓冲区装不下，在字符边界截断 */
        }
        memcpy(dst + used, ch_start, nbytes);
        used += nbytes;
        dst[used] = '\0';
        acc += adv;
    }

    return line + 1;
}
