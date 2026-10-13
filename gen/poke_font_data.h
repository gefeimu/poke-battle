/* 由 tools/lvgl_font_to_c.py 自动生成 —— 不要手工编辑 */
/*
 * 这里只有**数据**（位图、字形描述、码点表），查找与绘制在
 * src/poke_font.c 与 src/poke_render.c 里。
 *
 * 文件名特意不叫 poke_font.h：那会与 src/poke_font.h 重名，
 * 而 #include "poke_font.h" 在同目录优先，结果是生成的头文件
 * 包含自己，编译报 unknown type name（踩过一次）。
 */
#ifndef GEN_POKE_FONT_DATA_H
#define GEN_POKE_FONT_DATA_H

#include "poke_font.h"

/* font_app_16：4bpp，line_height=21，1282 条映射 */
extern const BitmapFont kFontApp16;
/* font_app_12：4bpp，line_height=16，1282 条映射 */
extern const BitmapFont kFontApp12;
/* font_hp_10：4bpp，line_height=12，95 条映射 */
extern const BitmapFont kFontHp10;

#endif /* GEN_POKE_FONT_DATA_H */
