/*
 * 绘制原语：RGB565 帧缓冲、矩形/圆角矩形/椭圆/条、4bpp 精灵图、位图字体。
 *
 * 定位
 * ----
 * 这是「逻辑 → 像素」的最下层，不含任何界面语义（不知道什么是血条框、
 * 什么是对话框）。界面层在 poke_ui.c。
 *
 * 为什么要自己写而不是用 LVGL
 * --------------------------
 * 1. **能在主机侧完整测试**。本项目的约定是所有逻辑都能在 PC 上跑断言
 *    （见 README），渲染也不例外：test/dump_screen.c 直接把画面导成图片，
 *    不需要上硬件。LVGL 需要一整套初始化，主机侧跑不起来。
 * 2. 界面只有 4 组、控件种类很少，原语十几个就够，引入 LVGL 不划算。
 * 3. 目标板无 PSRAM（380 KB SRAM），LVGL 的 widget 树 + 样式表是纯开销。
 *
 * 代价是控件要自己画。但界面样式已经按三代原版定好了（血条框 128x32 /
 * 128x44、对话框白底蓝框），用原语直接摆出来反而比用 widget 拼更准。
 *
 * 与 PC 预览的一致性
 * -----------------
 * tools/make_readme_images.py 用 Python 画同一套界面（README 的配图就是它
 * 出的）。两边是**独立实现**，但遵循同一份版式数值与同一套字体数据，
 * 所以可以互相比对 —— 见 test/dump_screen.c 里的说明。
 *
 * 部分刷新
 * -------
 * Surface 带 origin（缓冲左上角在屏幕上的坐标）与裁剪矩形，所以固件里可以
 * 只开一块 240xN 的条带缓冲、逐条渲染，不用一次性占 150 KB 全屏。
 */
#ifndef POKE_RENDER_H
#define POKE_RENDER_H

#include <stdbool.h>
#include <stdint.h>

#include "poke_font.h"
#include "poke_sprites.h"

/* ---------------------------------------------------------------- 颜色 */

/* RGB565。注意这是**小端字节序下的原始值**，送到 SPI 前可能要换字节
 * （官方 BSP 的 swap_bytes=true 就是干这个的）。 */
static inline uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b)
{
    return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

/* 把 RGB888 字面量转成 RGB565，写法上贴近设计稿里的 #RRGGBB */
#define RGB888(r, g, b) rgb565((uint8_t)(r), (uint8_t)(g), (uint8_t)(b))

/*
 * 纯位运算版本，给**静态初始化**用。
 *
 * rgb565() 是 static inline 函数，函数调用不是常量表达式，写成
 * `static const uint16_t k = RGB888(...)` 会编译不过。调色板常量
 * 都走这个宏。
 */
#define RGB565_C(r, g, b) \
    ((uint16_t)((((r) & 0xF8) << 8) | (((g) & 0xFC) << 3) | ((b) >> 3)))

uint16_t r_blend(uint16_t dst, uint16_t src, uint8_t alpha);

/* ---------------------------------------------------------------- 目标缓冲 */

typedef struct {
    uint16_t *pixels;
    int16_t   stride;     /* 一行多少像素（通常等于 bw，但也可能更大以对齐） */
    int16_t   ox, oy;     /* 缓冲左上角对应的屏幕坐标 */
    int16_t   bw, bh;     /* 缓冲的像素尺寸 */

    /*
     * 裁剪矩形，屏幕坐标，半开区间 [cx0, cx1) x [cy0, cy1)。
     * 所有绘制都会先与它求交，落在外面的一律不写 —— 这样「画出血条框」
     * 之类的越界不会污染相邻的内存，也让部分刷新能自然工作。
     */
    int16_t   cx0, cy0, cx1, cy1;
} Surface;

/* 用一整块缓冲初始化，裁剪区默认覆盖整块。stride 传 0 表示等于 buf_w。 */
void r_init(Surface *s, uint16_t *buf, int buf_w, int buf_h, int stride);

/* 缓冲在屏幕上的位置（部分刷新时用） */
void r_set_origin(Surface *s, int ox, int oy);

/* 设置/清除裁剪矩形（屏幕坐标，w/h <= 0 表示清成整块缓冲的范围） */
void r_set_clip(Surface *s, int x, int y, int w, int h);
void r_reset_clip(Surface *s);

/* ---------------------------------------------------------------- 图元 */

bool r_inside(const Surface *s, int x, int y);

void r_pixel(Surface *s, int x, int y, uint16_t c);
void r_fill(Surface *s, uint16_t c);
void r_fill_rect(Surface *s, int x, int y, int w, int h, uint16_t c);
void r_hline(Surface *s, int x, int y, int w, uint16_t c);
void r_vline(Surface *s, int x, int y, int h, uint16_t c);

/* 实心圆角矩形 */
void r_round_rect(Surface *s, int x, int y, int w, int h, int radius, uint16_t c);

/* 圆角矩形描边：先填 edge 色的整块，再用 fill 色填内缩 thickness 的一块。
 * fill 传 R_NO_FILL 表示只描边（内部不动）。 */
#define R_NO_FILL 0x10000u   /* 超出 RGB565 范围，用作哨兵 */
void r_round_rect_outline(Surface *s, int x, int y, int w, int h, int radius,
                          uint16_t edge, int thickness, uint32_t fill);

/* 实心椭圆（画站台用） */
void r_ellipse(Surface *s, int cx, int cy, int rx, int ry, uint16_t c);

/* 竖直渐变填充（天空用），逐行在 top -> bot 之间线性插值 */
void r_grad_v(Surface *s, int x, int y, int w, int h,
              uint16_t top, uint16_t bot);

/* 4bpp 精灵图。索引 0 = 透明，不绘制。越界部分自动裁掉。 */
void r_sprite(Surface *s, int x, int y, const SpriteEntry *spr);

/* ---------------------------------------------------------------- 文字 */

/*
 * 在 (x, y) 画一行文本，y 是**行框左上角**（不是基线）。
 * 返回画完后的 x（即下一段文本的起点），便于连续追加。
 *
 * 缺字（查不到字形）**不占宽度也不绘制**，与 font_text_width 一致。
 */
int r_text(Surface *s, int x, int y, const BitmapFont *f,
           const char *text, uint16_t color);

/* 带描边。小字号下描边能明显提升可读性（血条框里的 HP 标签就靠它）。 */
int r_text_outlined(Surface *s, int x, int y, const BitmapFont *f,
                    const char *text, uint16_t fill, uint16_t edge);

/* 一行文本的像素宽（内部累加 1/16 单位后一次取整） */
int r_text_width(const BitmapFont *f, const char *text);

#endif /* POKE_RENDER_H */
