/*
 * dump_screen.c - 用渲染层画出一整屏战斗界面，导出 PPM 图片
 *
 * 为什么要有这个
 * --------------
 * 精灵图和字库的教训是：**渲染问题只能看图发现**。数据错位、透明度算错、
 * 坐标差一格，编译不报错、断言也测不出来，但画出来一眼就能看出不对。
 *
 * 所以这个工具和 dump_sprites.c / dump_font.c 是同一套思路：把固件里
 * 真正会显示的像素导成图片，不上硬件就能验收。它是「渲染层最小闭环」
 * 的验收手段 —— 渲染层能独立画出一屏、导出的图与
 * tools/make_readme_images.py 出的 docs/img/hero.png 对得上，
 * 就说明引擎 → 界面 → 像素这条链路是通的。
 *
 * 与 Python 版的关系
 * ----------------
 * make_readme_images.py 是**独立实现**（Python + PIL），两边遵循同一份
 * 版式数值、同一套字体数据。所以对比时允许有量化差异（渐变插值、
 * 抗锯齿边缘的取整），但结构、位置、配色必须一致。
 *
 * 输出 PPM（P6，二进制 RGB888）而不是 PNG：PIL 原生支持 PPM，一行就能转，
 * 省得再写一个 PNG 编码器（之前手写 PNG 时踩过 zlib 存储块 64 KB 上限）。
 *
 * 用法：
 *   ./build/dump_screen [--out 输出路径.ppm]
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "poke_font_data.h"
#include "poke_render.h"
#include "poke_sprites.h"
#include "poke_ui.h"

#define W UI_SCREEN_W
#define H UI_SCREEN_H

/* 240*320*2 = 150 KB。主机侧随便开，固件里要按条带分块（见 poke_render.h）。 */
static uint16_t g_fb[W * H];

/* ---------------------------------------------------------------- 导出 */

static void write_ppm(const char *path, const uint16_t *fb, int w, int h)
{
    FILE *f = fopen(path, "wb");
    if (!f) {
        fprintf(stderr, "打不开 %s\n", path);
        exit(1);
    }
    fprintf(f, "P6\n%d %d\n255\n", w, h);

    for (int i = 0; i < w * h; i++) {
        uint16_t c = fb[i];
        int r = (c >> 11) & 0x1F;
        int g = (c >> 5) & 0x3F;
        int b = c & 0x1F;
        /*
         * 5/6/5 -> 8/8/8 用「左移补高位」而不是简单左移：
         * 只左移的话满值 31 会变成 248 而不是 255，白不够白。
         * 复制高位到低位后 0 -> 0、31 -> 255，两端都精确。
         */
        unsigned char rgb[3] = {
            (unsigned char)((r << 3) | (r >> 2)),
            (unsigned char)((g << 2) | (g >> 4)),
            (unsigned char)((b << 3) | (b >> 2)),
        };
        fwrite(rgb, 1, 3, f);
    }
    fclose(f);
}

/* ---------------------------------------------------------------- 字库覆盖 */

/*
 * 断言所有要画的字都在字库覆盖范围内。
 *
 * 字库的字符是从**真实数据（六张表的中文名）与真实文案（引擎日志）**
 * 收集的，所以任何临时编出来的中文都可能不在里面 —— 渲染出来是空白，
 * 不报错、不崩溃、图看着还挺正常。Python 版有同名检查（check_charset），
 * 第一版 hero 图就是靠它抓出「效果绝佳」的「佳」不在字库。
 */
static int check_charset(const BitmapFont *f, const char *label,
                         const char *const *strings, int n)
{
    if (!f) {
        return 0;
    }
    int bad = 0;
    for (int i = 0; i < n; i++) {
        if (!strings[i]) {
            continue;
        }
        const char *p = strings[i];
        while (*p) {
            uint32_t cp = font_utf8_next(&p);
            if (cp == ' ' || cp == '\n') {
                continue;
            }
            if (font_glyph_index(f, cp) == FONT_GLYPH_NONE) {
                fprintf(stderr, "  !! %s 用了字库没有的字：U+%04X（出自「%s」）\n",
                        label, cp, strings[i]);
                bad++;
            }
        }
    }
    return bad;
}

/* ---------------------------------------------------------------- main */

int main(int argc, char **argv)
{
    const char *out = "data/screen_preview/battle.ppm";
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--out") == 0 && i + 1 < argc) {
            out = argv[++i];
        }
    }

    /* ---- 组装一屏数据 ----
     * 内容与 make_readme_images.py 的 make_hero() 完全对应，这样两张图
     * 才能逐块对照。宝可梦名取自 gen/poke_species.c 的 name_cn。 */
    static const char *const foe_name = "巨金怪";
    static const char *const foe_lv = "Lv50";
    static const char *const me_name = "喷火龙";
    static const char *const me_lv = "Lv50";
    static const char *const me_hp = "68/153";

    BattleScreen d;
    memset(&d, 0, sizeof(d));

    d.foe_sprite = sprite_get(376, SPR_FRONT);   /* metagross */
    d.foe_name = foe_name;
    d.foe_lv = foe_lv;
    d.foe_hp_permil = 720;

    d.me_sprite = sprite_get(6, SPR_FRONT);      /* charizard */
    d.me_name = me_name;
    d.me_lv = me_lv;
    d.me_hp_permil = 440;
    d.me_hp_text = me_hp;

    static const char *const log0 = "喷火龙 使用了 喷射火焰！";
    static const char *const log1 = "会心一击！造成了 187 点伤害";
    d.log[0] = log0;
    d.log[1] = log1;

    static const char *const mv[4] = { "喷射火焰", "龙爪", "翅膀攻击", "剑舞" };
    static const char *const pp[4] = { "15", "15", "35", "20" };
    for (int i = 0; i < 4; i++) {
        d.moves[i] = mv[i];
        d.move_pp[i] = pp[i];
    }
    d.selected = 0;

    static const char *const st_l = "PP 15/15";
    static const char *const st_r = "选择招式   换人   逃跑";
    d.status_left = st_l;
    d.status_right = st_r;

    if (!d.foe_sprite || !d.me_sprite) {
        fprintf(stderr, "精灵图缺失（pid 376 或 6）\n");
        return 1;
    }

    /* ---- 字库覆盖检查（分三套字号各自检查）---- */
    int bad = 0;
    const char *box_strs[] = { foe_name, foe_lv, me_name, me_lv, me_hp };
    bad += check_charset(&kFontApp12, "血条框(12px)", box_strs, 5);
    bad += check_charset(&kFontHp10, "HP 标签(10px)", (const char *[]){ "HP" }, 1);
    const char *dlg_strs[] = { log0, log1, mv[0], mv[1], mv[2], mv[3],
                               pp[0], pp[1], pp[2], pp[3], st_l, st_r };
    bad += check_charset(&kFontApp16, "对话框(16px)", dlg_strs, 12);
    if (bad) {
        fprintf(stderr, "共 %d 处缺字，先把字库补上再出图\n", bad);
        return 1;
    }

    /* ---- 画 ---- */
    Surface s;
    r_init(&s, g_fb, W, H, W);
    r_fill(&s, 0);   /* 先涂黑：万一有没盖到的地方，图上一眼能看出来 */

    UiFonts fonts = {
        .box = &kFontApp12,
        .dialog = &kFontApp16,
        .label = &kFontHp10,
    };
    ui_battle_screen(&s, &fonts, &d);

    /* ---- 导出 ---- */
    write_ppm(out, g_fb, W, H);

    printf("已导出 %s（%dx%d）\n", out, W, H);
    printf("  对手 %s %s HP %d.%d%% 精灵图 %dx%d\n",
           foe_name, foe_lv, d.foe_hp_permil / 10, d.foe_hp_permil % 10,
           d.foe_sprite->w, d.foe_sprite->h);
    printf("  我方 %s %s HP %s（%d.%d%%）精灵图 %dx%d\n",
           me_name, me_lv, me_hp, d.me_hp_permil / 10, d.me_hp_permil % 10,
           d.me_sprite->w, d.me_sprite->h);
    printf("  字库：12px %u 字形 / 16px %u 字形 / 10px %u 字形\n",
           kFontApp12.glyph_count, kFontApp16.glyph_count,
           kFontHp10.glyph_count);
    printf("  分辨率：血条框 128x32（对手）+ 128x44（我方），对话框 232x52\n");
    return 0;
}
