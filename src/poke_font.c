/*
 * 位图字体的查找与度量。数据结构见 poke_font.h。
 */
#include "poke_font.h"

#include <stddef.h>

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
