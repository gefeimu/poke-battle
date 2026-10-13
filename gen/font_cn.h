/* 由 tools/gen_font.py 自动生成 */
#ifndef FONT_CN_H
#define FONT_CN_H

#include <stdint.h>

#define FONT_CELL_W 14
#define FONT_CELL_H 14
#define FONT_STRIDE 2   /* 每行字节数 = ceil(CELL_W/8)，行尾多余位为 0 */
#define FONT_GLYPH_COUNT 1280
#define FONT_ALIGN 8

/* 1bpp 位图，每字 28 字节（2 字节/行），行优先、高位在前 */
extern const uint8_t kFontBitmap[35840];

/* UTF-8 字符串首字节 -> 字形索引映射表，覆盖常用汉字 */
typedef struct {
    uint16_t codepoint;
    uint16_t glyph_index;
} FontMapEntry;

extern const FontMapEntry kFontMap[];
#define FONT_MAP_COUNT 1274

/* 按码点查字形索引，查不到返回 0xFFFF */
uint16_t font_lookup(uint32_t codepoint);

/* 绘制一段 UTF-8 文本到 RGB565 缓冲区 */
void font_draw_text(uint16_t *buf, int buf_w, int buf_h, int x, int y,
                   uint16_t color, const char *utf8);

#endif /* FONT_CN_H */