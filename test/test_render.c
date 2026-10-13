/*
 * test_render.c - 渲染层自检
 *
 * 渲染的 bug 和别处不同：**它不会崩、不会报错，只是画出来不对**。
 * 这一轮实际踩到的三个都是这个形态：
 *
 *   1. r_round_rect_outline 的「只描边」分支退化成了「把整块刷成描边色」，
 *      对话框的白底被内框线刷成了蓝底 —— 图上一眼能看出，但断言测不出。
 *   2. r_grad_v 往暗处插值时用了不对称的四舍五入，地面渐变偏亮 16 个色阶。
 *   3. 状态行的文字坐标写成了「相对框的偏移」而 PC 版用的是绝对坐标，
 *      结果整体右移 4px。
 *
 * 所以下面的断言分两类：
 *   * **可判定的**（不越界、返回值、查表边界、裁剪）—— 直接断言；
 *   * **只能看图发现的**（配色、版式）—— 用「取点验证」的方式断言：
 *     在已知位置取像素，检查它是不是预期的颜色。
 *     这类断言的价值是：改了绘制代码之后，如果某个区域从「填充色」
 *     变成了「描边色」，会立刻失败。
 *
 * 真正的肉眼验收仍然是 `make screen-preview` 导图看。
 */
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "poke_font_data.h"
#include "poke_render.h"
#include "poke_sprites.h"
#include "poke_ui.h"

#define W 240
#define H 320

static int g_pass = 0, g_fail = 0;

static void check(int cond, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    char buf[200];
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (cond) {
        g_pass++;
        printf("  [ OK ] %s\n", buf);
    } else {
        g_fail++;
        printf("  [FAIL] %s\n", buf);
    }
}

static uint16_t g_fb[W * H];

static Surface make(void)
{
    Surface s;
    memset(g_fb, 0, sizeof(g_fb));
    r_init(&s, g_fb, W, H, W);
    return s;
}

/* ---------------------------------------------------------------- 颜色 */

static void test_color(void)
{
    printf("\n[颜色] RGB565 与混合\n");

    check(rgb565(0, 0, 0) == 0x0000, "rgb565(0,0,0) = 0x0000");
    check(rgb565(255, 255, 255) == 0xFFFF, "rgb565(255,255,255) = 0xFFFF");
    check(rgb565(255, 0, 0) == 0xF800, "rgb565(255,0,0) = 0xF800（红）");
    check(rgb565(0, 255, 0) == 0x07E0, "rgb565(0,255,0) = 0x07E0（绿）");
    check(rgb565(0, 0, 255) == 0x001F, "rgb565(0,0,255) = 0x001F（蓝）");

    /* 常量版宏必须与函数版结果一致，否则调色板常量会对不上 */
    check(RGB565_C(255, 230, 0) == rgb565(255, 230, 0),
          "常量宏 RGB565_C 与 rgb565() 结果一致");
    check(RGB565_C(82, 106, 98) == rgb565(82, 106, 98),
          "常量宏在任意值上也一致");

    check(r_blend(0xFFFF, 0x0000, 0) == 0xFFFF, "alpha=0 保留底色");
    check(r_blend(0xFFFF, 0x0000, 255) == 0x0000, "alpha=255 取前景");
    check(r_blend(0xFFFF, 0x0000, 128) != 0xFFFF
          && r_blend(0xFFFF, 0x0000, 128) != 0x0000,
          "alpha=128 得到中间色");
}

/* ---------------------------------------------------------------- 裁剪 */

static void test_clip(void)
{
    printf("\n[裁剪] 越界不写、裁剪区生效\n");

    Surface s = make();
    r_fill_rect(&s, 10, 10, 10, 10, 0x1234);
    check(g_fb[10 * W + 10] == 0x1234, "矩形左上角被写入");
    check(g_fb[19 * W + 19] == 0x1234, "矩形右下角被写入");
    check(g_fb[9 * W + 10] == 0 && g_fb[10 * W + 9] == 0,
          "矩形外面没被写");

    /* 负坐标：不能越界写坏内存，也不能崩 */
    s = make();
    r_fill_rect(&s, -5, -5, 10, 10, 0x1234);
    check(g_fb[0] == 0x1234 && g_fb[4 * W + 4] == 0x1234,
          "负坐标矩形只画可见部分（不越界）");
    check(g_fb[5 * W + 5] == 0, "负坐标矩形在右下的部分是干净的");

    /* 完全在屏幕外 */
    s = make();
    r_fill_rect(&s, 300, 300, 10, 10, 0x1234);
    r_fill_rect(&s, -50, 100, 10, 10, 0x1234);
    int clean = 1;
    for (int i = 0; i < W * H; i++) {
        if (g_fb[i] != 0) { clean = 0; break; }
    }
    check(clean, "完全在屏幕外的矩形什么都不画");

    /* 裁剪区 */
    s = make();
    r_set_clip(&s, 20, 20, 10, 10);
    r_fill_rect(&s, 0, 0, W, H, 0x1234);
    check(g_fb[20 * W + 20] == 0x1234 && g_fb[29 * W + 29] == 0x1234,
          "裁剪区内被填充");
    check(g_fb[19 * W + 20] == 0 && g_fb[20 * W + 19] == 0,
          "裁剪区外没被填充");
    r_reset_clip(&s);
    r_fill_rect(&s, 0, 0, W, H, 0x4321);
    check(g_fb[0] == 0x4321, "reset 之后恢复全屏可写");

    /* 部分刷新：origin 偏移，绘制仍用屏幕坐标 */
    static uint16_t band[W * 20];
    memset(band, 0, sizeof(band));
    s = make();
    r_init(&s, band, W, 20, W);
    r_set_origin(&s, 0, 100);   /* 这条缓冲对应屏幕 y=100..119 */
    r_fill_rect(&s, 0, 100, W, 20, 0x1234);
    check(band[0] == 0x1234 && band[19 * W + W - 1] == 0x1234,
          "部分刷新：屏幕坐标 y=100 落到条带的第 0 行");
    r_fill_rect(&s, 0, 99, W, 1, 0x4321);
    check(band[0] == 0x1234, "部分刷新：条带外的一行没被写");
}

/* ---------------------------------------------------------------- 圆角 */

static void test_round_rect(void)
{
    printf("\n[圆角矩形] 半径收敛与描边语义\n");

    Surface s = make();
    r_round_rect(&s, 10, 10, 40, 30, 5, 0x1234);
    check(g_fb[10 * W + 10] == 0, "圆角矩形的外接角是空的（被切掉）");
    check(g_fb[10 * W + 30] == 0x1234, "上边中间是实心的");
    check(g_fb[25 * W + 10] == 0x1234, "左边中间是实心的");
    check(g_fb[25 * W + 30] == 0x1234, "内部是实心的");

    /* 半径超过一半要被收敛，不能算出负的内缩 */
    s = make();
    r_round_rect(&s, 0, 0, 10, 10, 999, 0x1234);
    check(g_fb[5 * W + 5] == 0x1234, "超大半径被收敛到 1/2 边长（中心仍是实心）");

    /* 正负号和 0 半径 */
    s = make();
    r_round_rect(&s, 10, 10, 10, 10, 0, 0x1234);
    check(g_fb[10 * W + 10] == 0x1234 && g_fb[19 * W + 19] == 0x1234,
          "半径 0 = 普通矩形（四角是实心的）");

    s = make();
    r_round_rect(&s, 10, 10, 10, 10, -3, 0x1234);
    check(g_fb[10 * W + 10] == 0x1234, "负半径当作 0 处理，不崩");

    s = make();
    r_round_rect(&s, 10, 10, 0, 10, 3, 0x1234);
    r_round_rect(&s, 10, 10, 10, 0, 3, 0x1234);
    check(1, "零宽/零高不崩，什么都不画");

    /*
     * R_NO_FILL 的语义回归 —— 这是本轮的真 bug。
     *
     * 原来「只描边」走的是「先铺满 edge 色、再用 fill 色挖空内部」那条路，
     * fill == R_NO_FILL 时挖空那步被跳过，整个矩形被刷成描边色，
     * 内部原有的内容全丢。对话框的内框线就是这么把白底刷成蓝底的。
     */
    s = make();
    r_fill_rect(&s, 0, 0, 60, 40, 0xFFFF);          /* 先铺一层「白底」 */
    r_round_rect_outline(&s, 10, 10, 40, 20, 4, 0x1234, 2, R_NO_FILL);
    check(g_fb[20 * W + 30] == 0xFFFF,
          "R_NO_FILL 时内部**保持原样**（这个 bug 曾把对话框白底刷成蓝底）");
    check(g_fb[10 * W + 30] == 0x1234, "R_NO_FILL 时边框被画上");
    check(g_fb[11 * W + 30] == 0x1234, "边框有 thickness 那么厚");
    check(g_fb[12 * W + 30] == 0xFFFF, "边框内侧立刻回到原内容");

    /* 带填充时两条路都必须得到同样的结果 */
    s = make();
    r_round_rect_outline(&s, 10, 10, 40, 20, 4, 0x1234, 2, 0xFFFF);
    check(g_fb[20 * W + 30] == 0xFFFF && g_fb[10 * W + 30] == 0x1234,
          "带填充时：边框 + 内部填充都正确");
}

/* ---------------------------------------------------------------- 渐变 */

static void test_grad(void)
{
    printf("\n[渐变] 往暗处插值不能偏亮\n");

    Surface s = make();
    /* 亮 -> 暗。曾经这里用了不对称的四舍五入，末端偏亮 16 个色阶。 */
    uint16_t top = rgb565(152, 200, 112);
    uint16_t bot = rgb565(120, 176, 88);
    r_grad_v(&s, 0, 0, 10, 101, top, bot);

    check(g_fb[0] == top, "渐变首行 = 起始色");
    check(g_fb[100 * W] == bot, "渐变末行 = 结束色");

    int mid = (g_fb[50 * W] >> 11) & 0x1F;
    int lo = (bot >> 11) & 0x1F;
    int hi = (top >> 11) & 0x1F;
    check(mid <= hi && mid >= lo, "中间行落在两端之间（R 通道 %d 在 [%d,%d]）",
          mid, lo, hi);

    /* 逐行单调（往暗处必须单调不增，不能因为取整而回弹） */
    int mono = 1;
    for (int y = 1; y < 101; y++) {
        if ((g_fb[y * W] >> 11 & 0x1F) > (g_fb[(y - 1) * W] >> 11 & 0x1F)) {
            mono = 0;
            break;
        }
    }
    check(mono, "往暗处渐变逐行单调不增");

    /* 高度 1 的退化情形 */
    s = make();
    r_grad_v(&s, 0, 0, 10, 1, top, bot);
    check(g_fb[0] == top, "高度 1 时取起始色，不除零");
}

/* ---------------------------------------------------------------- 精灵图 */

static void test_sprite(void)
{
    printf("\n[精灵图] 透明索引与越界\n");

    const SpriteEntry *e = sprite_get(6, SPR_FRONT);
    check(e != NULL, "能用 sprite_get 取到 pid=6 的正面图");
    if (!e) {
        return;
    }
    check(e->w == 64 && e->h == 64, "尺寸 64x64（实际 %dx%d）", e->w, e->h);
    check(sprite_get(64000, SPR_FRONT) == NULL, "不存在的 pid 返回 NULL");

    /* 数一数透明像素：应当有一些（否则就是没有透明背景） */
    int transparent = 0;
    for (int i = 0; i < e->w * e->h; i++) {
        if (((e->data[i / 2] >> ((i % 2) ? 0 : 4)) & 0x0F) == PAL_TRANSPARENT) {
            transparent++;
        }
    }
    check(transparent > 0, "有透明像素（%d 个）", transparent);
    check(transparent < e->w * e->h, "不是整张全透明");

    /* 透明像素必须不绘制：先铺底色，再贴图，找第一个透明像素检查 */
    Surface s = make();
    r_fill_rect(&s, 0, 0, W, H, 0xABCD);
    r_sprite(&s, 10, 10, e);
    int ok_transparent = 1;
    for (int gy = 0; gy < e->h && ok_transparent; gy++) {
        for (int gx = 0; gx < e->w; gx++) {
            int i = gy * e->w + gx;
            if (((e->data[i / 2] >> ((i % 2) ? 0 : 4)) & 0x0F) == PAL_TRANSPARENT) {
                if (g_fb[(10 + gy) * W + 10 + gx] != 0xABCD) {
                    ok_transparent = 0;
                }
                break;   /* 每行找一个就够 */
            }
        }
    }
    check(ok_transparent, "透明索引处保留底色（不绘制）");

    /* 越界：画到屏幕外不能崩、不能写坏内存 */
    s = make();
    r_sprite(&s, -30, -20, e);
    r_sprite(&s, W - 10, H - 10, e);
    r_sprite(&s, 10000, 10000, e);
    check(1, "精灵图越界/完全在外都不崩");

    /* NULL 安全 */
    s = make();
    r_sprite(&s, 0, 0, NULL);
    check(1, "传 NULL 精灵图不崩");
}

/* ---------------------------------------------------------------- 字体 */

static void test_font(void)
{
    printf("\n[字体] 查表边界与度量\n");

    check(font_glyph_index(&kFontApp16, 'A') != FONT_GLYPH_NONE, "16px 有 'A'");
    check(font_glyph_index(&kFontApp16, 0x5510 /* 唐，肯定没有 */)
          == FONT_GLYPH_NONE, "16px 查不到的字返回 NONE");
    /* 注意用码点而不是 '妙'：C 里多字节字符常量是 int（UTF-8 字节拼合），
     * 不是 Unicode 码点，写上会得到一个错误的值且编译器只给 warning。 */
    check(font_glyph_index(&kFontApp16, 0x5999) != FONT_GLYPH_NONE,
          "16px 有汉字「妙」(U+5999)");

    /* 码点表必须升序 —— 二分查找的前提，生成时排好但要有断言钉住 */
    int sorted = 1;
    for (int i = 1; i < kFontApp16.map_count; i++) {
        if (kFontApp16.map[i].codepoint <= kFontApp16.map[i - 1].codepoint) {
            sorted = 0;
            break;
        }
    }
    check(sorted, "16px 的码点表严格升序（二分查找的前提）");

    sorted = 1;
    for (int i = 1; i < kFontApp12.map_count; i++) {
        if (kFontApp12.map[i].codepoint <= kFontApp12.map[i - 1].codepoint) {
            sorted = 0;
            break;
        }
    }
    check(sorted, "12px 的码点表严格升序");
    check(kFontApp12.line_height == 16 && kFontHp10.line_height == 12,
          "三套字体的行高：16px=%d 12px=%d 10px=%d",
          kFontApp16.line_height, kFontApp12.line_height, kFontHp10.line_height);

    /* 索引 0 是保留槽，不能返回字形 */
    check(font_glyph(&kFontApp16, 0) == NULL, "索引 0 是保留槽，取不到字形");
    check(font_glyph(&kFontApp16, kFontApp16.glyph_count) == NULL,
          "索引越界取不到字形");

    /* 度量 */
    check(font_text_width(&kFontApp16, "") == 0, "空串宽度 0");
    check(font_text_width(&kFontApp16, "中") > 0, "汉字宽度 > 0");
    check(font_text_width(&kFontApp16, "中中") ==
          2 * font_text_width(&kFontApp16, "中"),
          "两个相同汉字 = 单字两倍（等宽）");
    check(font_text_width(&kFontApp16, "A") < font_text_width(&kFontApp16, "中"),
          "ASCII 比汉字窄（比例宽度）");
    /* 缺字不占宽度 */
    check(font_text_width(&kFontApp16, "A\u5510") ==
          font_text_width(&kFontApp16, "A"),
          "缺字不占宽度");

    /* UTF-8 解码 */
    const char *p = "妙";
    check(font_utf8_next(&p) == 0x5999, "UTF-8 解码 3 字节汉字");
    p = "A";
    check(font_utf8_next(&p) == 'A', "UTF-8 解码 ASCII");
    p = "\xFF";                      /* 非法字节 */
    check(font_utf8_next(&p) == 0xFF, "非法字节按单字节处理");
}

static void test_text_draw(void)
{
    printf("\n[文字绘制] 返回值与缺字\n");

    Surface s = make();
    r_fill_rect(&s, 0, 0, W, H, 0x0000);

    int x0 = 20, y0 = 20;
    int x1 = r_text(&s, x0, y0, &kFontApp16, "妙蛙种子", 0xFFFF);
    check(x1 - x0 == font_text_width(&kFontApp16, "妙蛙种子"),
          "r_text 返回的末尾 x 等于测得的宽度（%d vs %d）",
          x1 - x0, font_text_width(&kFontApp16, "妙蛙种子"));

    /* 画上去了吗 */
    int drawn = 0;
    for (int y = y0; y < y0 + 24; y++) {
        for (int x = x0; x < x1; x++) {
            if (g_fb[y * W + x] != 0) {
                drawn++;
            }
        }
    }
    check(drawn > 50, "文字确实画出了像素（%d 个）", drawn);

    /* 缺字：不画也不占宽度 */
    s = make();
    int xa = r_text(&s, 10, 10, &kFontApp16, "A\u5510B", 0xFFFF);
    int wa = font_text_width(&kFontApp16, "AB");
    check(xa - 10 == wa, "整行含缺字时宽度只算有字形的部分");

    /* 越界绘制不崩 */
    s = make();
    r_text(&s, -100, -100, &kFontApp16, "妙蛙种子", 0xFFFF);
    r_text(&s, W - 2, H - 2, &kFontApp16, "妙蛙种子", 0xFFFF);
    r_text(&s, 0, 0, NULL, "x", 0xFFFF);
    r_text(&s, 0, 0, &kFontApp16, NULL, 0xFFFF);
    check(1, "越界/空指针都不崩");

    /* 描边：四向各偏移 1px，所以占地比不描边大 */
    s = make();
    int xo = r_text_outlined(&s, 20, 20, &kFontHp10, "HP", 0xFFFF, 0x0000);
    check(xo == 20 + font_text_width(&kFontHp10, "HP"),
          "r_text_outlined 的返回值与不加描边一致");
}

/* ---------------------------------------------------------------- 界面取点 */

/*
 * 界面样式没法用「断言值」来测（好不好看是主观的），但可以**取点验证**：
 * 在版式表规定的位置取像素，检查它是不是预期的颜色。
 * 这样改了绘制代码之后，某个区域从「填充色」变成「描边色」会立刻失败。
 */
static void test_ui_pixels(void)
{
    printf("\n[界面取点] 版式表里规定的位置应有规定的内容\n");

    static const char *const foe_name = "巨金怪";
    static const char *const me_name = "喷火龙";
    static const char *const me_hp = "68/153";
    static const char *const mv[4] = { "喷射火焰", "龙爪", "翅膀攻击", "剑舞" };
    static const char *const pp[4] = { "15", "15", "35", "20" };
    static const char *const log0 = "喷火龙 使用了 喷射火焰！";

    BattleScreen d;
    memset(&d, 0, sizeof(d));
    d.foe_sprite = sprite_get(376, SPR_FRONT);
    d.me_sprite = sprite_get(6, SPR_FRONT);
    d.foe_name = foe_name;
    d.me_name = me_name;
    d.foe_lv = "Lv50";
    d.me_lv = "Lv50";
    d.foe_hp_permil = 720;
    d.me_hp_permil = 440;
    d.me_hp_text = me_hp;
    d.log[0] = log0;
    d.log[1] = NULL;
    for (int i = 0; i < 4; i++) {
        d.moves[i] = mv[i];
        d.move_pp[i] = pp[i];
    }
    d.selected = 0;

    Surface s;
    memset(g_fb, 0, sizeof(g_fb));
    r_init(&s, g_fb, W, H, W);

    UiFonts f = { .box = &kFontApp12, .dialog = &kFontApp16,
                  .label = &kFontHp10 };
    ui_battle_screen(&s, &f, &d);

    /* 血条框：左上角内缩 2px，所以 (2,2) 附近应是描边色 */
    check(g_fb[2 * W + 2] != 0, "对手血条框左上角有内容（不是背景）");
    /* 框内应是米白填充 */
    check(g_fb[8 * W + 60] == RGB565_C(255, 255, 222),
          "对手血条框内部是米白填充");
    check(g_fb[115 * W + 180] == RGB565_C(255, 255, 222),
          "我方血条框内部是米白填充");
    /* 框外左上角（0,0 附近）应是天空色系，不是框 */
    check(g_fb[0] != RGB565_C(255, 255, 222),
          "屏幕左上角 (0,0) 不是血条框（留了 2px 缝）");

    /* 对话框内部应是白的 */
    check(g_fb[200 * W + 120] == RGB565_C(255, 255, 255),
          "对话框内部是白色（R_NO_FILL 的 bug 会把它刷成蓝）");

    /* 招式选中项是黄底。取 y=227（格子顶边 225 往下 2px）——
     * 文字墨迹从 230 才开始，这一行是纯底色，不会被文字干扰。 */
    check(g_fb[227 * W + 60] == RGB565_C(255, 230, 57),
          "选中的招式是黄底");
    /* 未选中项是白底 */
    check(g_fb[227 * W + 170] == RGB565_C(255, 255, 255),
          "未选中的招式是白底");

    /* 状态行内部是白的 */
    check(g_fb[300 * W + 120] == RGB565_C(255, 255, 255),
          "状态行内部是白色");

    /* 地面：下半屏应当是绿色系（R 通道明显小于 G） */
    uint16_t ground = g_fb[250 * W + 120];
    int gr = (ground >> 11) & 0x1F, gg = (ground >> 5) & 0x3F;
    check(gg > gr, "下半屏背景是绿色系（G > R）");

    /*
     * 天空：上半屏应当是蓝色系。
     * 判据用「R 最小」而不是「B 最大」—— 天空是**浅蓝**（108,176,232），
     * 绿的位宽是 6 bit、分量本来就比蓝大，按「B 最大」判会误报。
     * 蓝色系的稳定特征是红分量明显最低。
     */
    uint16_t sky = g_fb[60 * W + 5];
    int sb = sky & 0x1F, sg = (sky >> 5) & 0x3F, sr = (sky >> 11) & 0x1F;
    check(sr < sg && sr < sb, "上半屏背景是蓝色系（R 分量最低，实测 %d/%d/%d）",
          sr, sg, sb);

    /* HP 标签是金色，在条的左端 */
    int found_gold = 0;
    for (int x = 10; x < 30; x++) {
        for (int y = 18; y < 28; y++) {
            if (g_fb[y * W + x] == RGB565_C(255, 230, 0)) {
                found_gold++;
            }
        }
    }
    check(found_gold > 0, "对手框的 HP 条左端有金色 HP 标签（%d 个像素）",
          found_gold);
}

/* ---------------------------------------------------------------- 菜单取点 */

/*
 * 菜单界面的取点验证。与上面的战斗界面同一思路：版式是本项目自己定的
 * （没有原版可抄），所以断言的不是「像不像原版」而是「各元素在它该在的
 * 位置、且没有互相压住」。
 *
 * 这里刻意把期望坐标写成**字面量**而不是引用 poke_ui.c 里的宏 —— 那些宏
 * 是 .c 的私有实现，测试引用它们就测不出「实现自己改了」。字面量写死，
 * 改版式的人会在这里撞墙，被迫回头确认每一处。
 */
static void test_menu_pixels(void)
{
    printf("\n[菜单取点] 列表 / 弹层 / 结算的版式\n");

    static const char *const items[3] = { "开始对战", "队伍配置", "第三项" };
    UiFonts f = { .box = &kFontApp12, .dialog = &kFontApp16,
                  .label = &kFontHp10 };

    /* ---- 整屏列表：3 项，选中第 2 项 ---- */
    MenuScreen m;
    memset(&m, 0, sizeof(m));
    m.title = "主菜单";
    for (int i = 0; i < 3; i++) {
        m.items[i] = items[i];
    }
    m.count = 3;
    m.rows = 3;
    m.selected = 1;
    m.status_left = "2/3";
    m.status_right = "选择   确定";

    Surface s;
    memset(g_fb, 0, sizeof(g_fb));
    r_init(&s, g_fb, W, H, W);
    ui_menu_screen(&s, &f, &m);

    /* 标题横条：深蓝填充。横条 6..38，内缩 2px 后 8..36。 */
    check(g_fb[20 * W + 8] == RGB565_C(41, 74, 123), "标题横条是深蓝填充");
    /* 标题与列表板之间（y=42）应露出背景：天空是浅蓝，R 分量最低 */
    uint16_t gap = g_fb[42 * W + 120];
    int gr = (gap >> 11) & 0x1F, gg = (gap >> 5) & 0x3F, gb = gap & 0x1F;
    check(gr < gg && gr < gb, "标题与列表板之间是背景（R 最低，实测 %d/%d/%d）",
          gr, gg, gb);

    /* 列表板：44 起，高 = 上下各 6 + 3 行 × 28 = 96，即 44..140 */
    check(g_fb[52 * W + 200] == RGB565_C(255, 255, 255),
          "未选中的行是白底（板内）");
    /* 第 2 行 = y 78，高亮块 77..102、内缩 2px 后 79..100 */
    check(g_fb[85 * W + 200] == RGB565_C(255, 230, 57), "选中的行是黄底");
    check(g_fb[107 * W + 200] == RGB565_C(255, 255, 255),
          "选中行下面那行仍是白底");
    /* 板外（y=150）应回到背景 */
    uint16_t below = g_fb[150 * W + 120];
    int br = (below >> 11) & 0x1F;
    int bg2 = (below >> 5) & 0x3F;
    check(br < bg2, "列表板随内容长高，板下面是背景而不是空白板");

    /* 状态行与战斗界面同一处版式：内部是白的。
     * 取点要避开文字：左侧 "2/3" 到 x≈32，右侧提示从 STATUS_TEXT_X=100 起，
     * 所以 x=60 这一段是纯底色。 */
    check(g_fb[300 * W + 60] == RGB565_C(255, 255, 255), "状态行内部是白色");

    /* ---- 滚动条：386 项、一屏 8 行 ---- */
    MenuScreen big;
    memset(&big, 0, sizeof(big));
    big.title = "队伍配置";
    big.count = 386;
    big.rows = UI_MENU_ROWS_MAX;
    big.selected = 0;
    big.scrollbar = true;
    big.status_right = "滚动   确定   长按返回";

    memset(g_fb, 0, sizeof(g_fb));
    r_init(&s, g_fb, W, H, W);
    ui_menu_screen(&s, &f, &big);

    /* 轨道 x = 4 + 232 - 9 = 227，宽 5；滑块最顶时 52 起（轨道 52..272） */
    check(g_fb[54 * W + 229] == RGB565_C(65, 74, 123),
          "滚动条滑块在顶部（top=0）");

    big.top = big.count - big.rows;      /* 滚到底 */
    memset(g_fb, 0, sizeof(g_fb));
    r_init(&s, g_fb, W, H, W);
    ui_menu_screen(&s, &f, &big);
    check(g_fb[54 * W + 229] != RGB565_C(65, 74, 123),
          "滚到底后顶部不再是滑块");
    /* 滑块高 = max(10, 220*8/386=4) = 10，贴着轨道底 272，即 262..272 */
    check(g_fb[265 * W + 229] == RGB565_C(65, 74, 123),
          "滚到底后滑块贴着轨道底端");

    /* ---- 结算：大字板的配色由结果决定 ---- */
    MenuScreen res;
    memset(&res, 0, sizeof(res));
    res.title = "结果";
    res.big = "胜利";
    res.big_style = UI_BIG_WIN;
    res.status_right = "确定继续";

    memset(g_fb, 0, sizeof(g_fb));
    r_init(&s, g_fb, W, H, W);
    ui_menu_screen(&s, &f, &res);
    check(g_fb[160 * W + 60] == RGB565_C(214, 60, 52), "胜利是红底");

    res.big_style = UI_BIG_LOSE;
    memset(g_fb, 0, sizeof(g_fb));
    r_init(&s, g_fb, W, H, W);
    ui_menu_screen(&s, &f, &res);
    check(g_fb[160 * W + 60] == RGB565_C(58, 96, 168), "失败是蓝底");

    /* ---- 弹层：盖在战斗界面之上 ---- */
    BattleScreen d;
    memset(&d, 0, sizeof(d));
    d.foe_sprite = sprite_get(376, SPR_FRONT);
    d.me_sprite = sprite_get(6, SPR_FRONT);
    d.foe_name = "巨金怪";
    d.me_name = "喷火龙";
    d.foe_lv = "Lv50";
    d.me_lv = "Lv50";
    d.foe_hp_permil = 720;
    d.me_hp_permil = 990;
    d.log[0] = "对手 派出了 巨金怪！";
    for (int i = 0; i < 4; i++) {
        d.moves[i] = items[0];
        d.move_pp[i] = "15";
    }
    d.selected = 0;

    MenuScreen ovl;
    memset(&ovl, 0, sizeof(ovl));
    ovl.title = "选择";
    ovl.items[0] = "换人";
    ovl.items[1] = "认输";
    ovl.count = 2;
    ovl.rows = 2;
    ovl.selected = 0;
    ovl.status_right = "选择   确定   长按返回";

    /* ui_overlay_rect 必须正好是招式菜单的矩形：弹层就是靠「完全盖住它」
     * 来分层的。这四个数字写死 —— 改了招式版式而忘了改弹层，这里立刻断。 */
    int rx, ry, rw, rh;
    ui_overlay_rect(&rx, &ry, &rw, &rh);
    check(rx == 4 && ry == 220 && rw == 232 && rh == 64,
          "弹层矩形 = 招式菜单矩形（实测 %d,%d %dx%d）", rx, ry, rw, rh);

    memset(g_fb, 0, sizeof(g_fb));
    r_init(&s, g_fb, W, H, W);
    ui_battle_screen(&s, &f, &d);
    ui_overlay_screen(&s, &f, &ovl);

    /* 金色模态外框：框外 2px 起，左边 x=2 */
    check(g_fb[250 * W + 2] == RGB565_C(205, 172, 8), "弹层有金色模态外框");
    /* 框内标题行是白的（y=224 在 12px 标题行的行框里，x=200 无墨迹） */
    check(g_fb[224 * W + 200] == RGB565_C(255, 255, 255), "弹层标题行是白底");
    /* 第 1 行选中：y 236 起，高亮 235..257、内缩后 237..255 */
    check(g_fb[245 * W + 200] == RGB565_C(255, 230, 57), "弹层选中项是黄底");
    /* 第 2 行白底 */
    check(g_fb[262 * W + 200] == RGB565_C(255, 255, 255), "弹层未选中项是白底");

    /*
     * 招式菜单必须被**完全盖住**。招式菜单第 2 列格子的浅蓝描边在
     * (124..230, 225) —— 弹层画上去之后那里应是弹层自己的内容（标题行的
     * 白底），不能再看到招式格子的描边色。漏画 ui_overlay_screen 或把
     * 弹层画小了，这里都会露馅。
     */
    check(g_fb[225 * W + 200] != RGB565_C(106, 139, 189),
          "弹层盖住了招式菜单（那里不再是招式格子的描边色）");
}

int main(void)
{
    printf("渲染层自检\n");
    printf("==========\n");

    test_color();
    test_clip();
    test_round_rect();
    test_grad();
    test_sprite();
    test_font();
    test_text_draw();
    test_ui_pixels();
    test_menu_pixels();

    printf("\n=== 结果: %d 通过, %d 失败 ===\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
