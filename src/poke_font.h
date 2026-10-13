/*
 * 位图字体的数据结构。
 *
 * 为什么不直接用 lv_font_conv 生成的 .c
 * --------------------------------------
 * 那份文件有两个问题：
 *   1. 数组是 `static` 的，外部拿不到；
 *   2. 依赖 lvgl.h（LV_ATTRIBUTE_LARGE_CONST / lv_font_t / cmap 结构体 …）。
 *
 * src/ 的约定是**零依赖的纯 C，能在主机侧完整测试**（见 README），
 * 所以用 tools/lvgl_font_to_c.py 把 lv_font_conv 的输出转成本结构。
 * 转换只做「重新排列 + 展平 codepoint 查找表」，字形数据一字不改，
 * 所以画出来的东西与 LVGL 渲染的一致（tools/render_lvgl_font.py 是
 * 独立实现，两边可互相对照）。
 *
 * 与自制 1bpp 字库（gen/font_cn.c）的关系
 * --------------------------------------
 * 那是另一套独立的字库（14x14 点阵、自带 font_lookup/font_draw_text），
 * 保留着做对比。本结构更通用：支持 1/2/4/8 bpp、任意字号、多套字体。
 */
#ifndef POKE_FONT_H
#define POKE_FONT_H

#include <stddef.h>
#include <stdint.h>

/* 查不到字形时的返回值 */
#define FONT_GLYPH_NONE 0xFFFF

/*
 * 单个字形。
 *
 * `bitmap_index` 是**字节偏移**（不是位偏移）。4bpp 字形的位图是
 * **连续位打包**的：一个 box_w x box_h 的字形占 ceil(w*h*bpp/8) 字节，
 * 行与行之间不补齐字节。这条与 LVGL 一致，也已被 40 个连续字形验证过
 * （见 README 的「LVGL 4bpp 的格式约定」）。按「每行对齐」解会全错。
 */
typedef struct {
    uint32_t bitmap_index;
    uint8_t  adv_w;   /* 步进宽度，单位 1/16 像素（沿用 LVGL 的原始单位，避免取整误差累积） */
    uint8_t  box_w;   /* 字形墨水宽 */
    uint8_t  box_h;   /* 字形墨水高 */
    int8_t   ofs_x;   /* 墨水左边缘相对光标的偏移 */
    int8_t   ofs_y;   /* 墨水底边相对基线的偏移，向上为正 */
} GlyphDsc;

/*
 * 码点 -> 字形索引。
 *
 * 这张表是**展平**的：lv_font_conv 原本用「区间 + 稀疏列表」的紧凑表示
 * （cmaps + unicode_list），这里摊平成每个码点一条，渲染时二分查找。
 * 代价是每个码点多占 6 字节（1283 条约 10 KB），换来的是查找逻辑只有
 * 十几行、不需要理解四种 cmap 格式。8 MB Flash 里这点开销可以忽略。
 *
 * **表必须按 codepoint 升序**，二分查找依赖它（test_render.c 会断言）。
 */
typedef struct {
    uint32_t codepoint;
    uint16_t glyph_index;
} FontMapEntry;

typedef struct {
    const char *name;
    uint8_t  bpp;           /* 1 / 2 / 4 / 8 */
    uint8_t  line_height;   /* 行高 */
    int8_t   base_line;     /* 基线距行底的距离（往上数） */
    uint16_t glyph_count;   /* 字形数，含索引 0 的保留槽 */
    uint16_t map_count;     /* 映射表条数 */
    const uint8_t *bitmap;
    const GlyphDsc *glyphs;
    const FontMapEntry *map;
} BitmapFont;

/* 按码点查字形索引，找不到返回 FONT_GLYPH_NONE */
uint16_t font_glyph_index(const BitmapFont *f, uint32_t cp);

/* 取某个字形的描述；索引越界或为保留槽时返回 NULL */
const GlyphDsc *font_glyph(const BitmapFont *f, uint16_t index);

/*
 * UTF-8 解码：读懂 *p 处的一个字符，返回码点并把 *p 前移到下一个字符。
 * 非法字节按单字节处理（返回该字节值），保证不会死循环。
 * 与 gen/font_cn.c 里的实现行为一致。
 */
uint32_t font_utf8_next(const char **p);

/* 一行文本的像素宽度（adv_w 累加后一次取整，避免逐字取整的误差累积） */
int font_text_width(const BitmapFont *f, const char *text);

/*
 * 贪心折行：把 text 折成最多两行，写进 l0 / l1（UTF-8，各自带终止符）。
 * 返回**实际需要的行数**：
 *   0  text 为空（两行都写成空串）
 *   1  一行放得下
 *   2  折成两行放得下
 *   >2 两行仍放不下（l1 里是剩余部分，会超出 max_w）
 *
 * 最后那条是刻意留的信息而不是错误 —— 战斗日志的对话框只有两行，
 * 「这段文字塞不进这个框」必须能被断言出来，而不是画出去才发现被裁掉。
 *
 * 断行只发生在字符之间（绝不把一个多字节字符劈开），也不需要英文那套
 * 词边界规则：引擎的日志是中文短句，按字符断就够。空格按原宽占位。
 *
 * max_w 必须 > 0。**每行的第一个字符无条件放下**，否则一个比整行还宽的
 * 字会让循环反复换行而死循环。
 */
int font_wrap2(const BitmapFont *f, const char *text, int max_w,
               char *l0, size_t cap0, char *l1, size_t cap1);

#endif /* POKE_FONT_H */
