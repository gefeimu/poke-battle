/*
 * dump_screen.c - 把固件里真正会显示的像素导成 PPM 图片
 *
 * 为什么要有这个
 * --------------
 * 精灵图和字库的教训是：**渲染问题只能看图发现**。数据错位、透明度算错、
 * 坐标差一格，编译不报错、断言也测不出来，但画出来一眼就能看出不对。
 *
 * 所以这个工具和 dump_sprites.c / dump_font.c 是同一套思路：把固件里
 * 真正会显示的像素导成图片，不上硬件就能验收。
 *
 * 七屏都从这里出
 * --------------
 * 路线图第 4 项（菜单绘制）之后，界面上能出现的屏一共七种。**每一屏都能
 * 单独导出**，因为它们各自的版式不同源（战斗界面照原版，菜单是自己定的），
 * 混在一起看容易漏掉某一屏没画：
 *
 *   main     主菜单（标题横条 + 两项列表）
 *   team     队伍配置（386 项列表 + 滚动条 + "2/386" 计数）
 *   log      日志播放（战斗界面 + 对话框逐条放战况）
 *   battle   战斗界面（招式菜单 2x2）—— 见下面「为什么这张是手工数据」
 *   action   弹层（换人 / 认输，盖在招式框上）
 *   confirm  认输确认（取消 / 认输）
 *   switch   换人（整屏队伍列表，每行挂当前 HP）
 *   result   结算（胜利 / 失败 / 平局大字）
 *
 * 除了 battle，**其余六屏的数据都是从真实会话跑出来的**，不是手摆的：
 * 喂一串按键走到那一屏，再调 ses_view() / ses_menu_view() 拿数据。这样
 * 导出的图就是设备上会显示的那一屏 —— 会话层的映射错了，图上也会跟着错，
 * 而不是「手摆的数据画得挺好看，真机上却是另一回事」。
 *
 * 为什么 battle 那张是手工数据
 * ---------------------------
 * 它要和 tools/make_readme_images.py 出的 docs/img/hero.png **逐像素对照**
 * （make render-compare）。对照要求两边画出完全相同的画面，而满血开局的
 * 真机画面里血量是 100%、也没有「会心一击」这类日志 —— hero 图是**示意**
 * 用的，血量与日志都是摆出来的。所以这一屏保留手工数据，其余六屏走真数据。
 *
 * 为了让结果可重放，这里覆盖了 battle_rng() / calc_rand_85_100()（与
 * test_session.c 同一套桩）。对手是 AI，不钉住抽签的话每次导出的图都不一样。
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
 *   ./build/dump_screen                      导 battle（render-compare 用这个）
 *   ./build/dump_screen --screen action      导某一屏
 *   ./build/dump_screen --all                七屏全导到 data/screen_preview/
 *   ./build/dump_screen --list               列出可导的屏
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "poke_font_data.h"
#include "poke_render.h"
#include "poke_session.h"
#include "poke_sprites.h"
#include "poke_ui.h"

#define W UI_SCREEN_W
#define H UI_SCREEN_H

/* 240*320*2 = 150 KB。主机侧随便开，固件里要按条带分块（见 poke_render.h）。 */
static uint16_t g_fb[W * H];

/* ================================================================ 固定随机源 */

/* 与 test_session.c 同一套 xorshift 桩：图要可重放，不然每次导出都不一样。
 * -DPOKE_BATTLE_TEST_NO_RNG / -DPOKE_CALC_TEST_NO_RAND 关掉了引擎内的实现。 */
static uint32_t s_rng = 0x12345678u;

uint32_t battle_rng(void)
{
    s_rng ^= s_rng << 13;
    s_rng ^= s_rng >> 17;
    s_rng ^= s_rng << 5;
    return s_rng;
}

uint8_t calc_rand_85_100(void)
{
    s_rng ^= s_rng << 13;
    s_rng ^= s_rng >> 17;
    s_rng ^= s_rng << 5;
    return (uint8_t)(85 + (s_rng % 16));
}

static void rng_reset(void) { s_rng = 0x12345678u; }

/* ================================================================ 导出 */

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

/* ================================================================ 字库覆盖 */

/*
 * 断言所有要画的字都在字库覆盖范围内。
 *
 * 字库的字符是从**真实数据（六张表的中文名）与真实文案（引擎日志、
 * 界面文案）**收集的，所以任何临时编出来的中文都可能不在里面 —— 渲染出来
 * 是空白，不报错、不崩溃、图看着还挺正常。Python 版有同名检查（check_charset），
 * 第一版 hero 图就是靠它抓出「效果绝佳」的「佳」不在字库。
 *
 * 菜单绘制这一轮也靠它抓到过：状态机里的「主菜单」「队伍配置」六个字
 * （主菜队伍配）压根不在字库里 —— 状态机写完没重跑 `make font`。
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

/*
 * 把这一屏用到的所有文字收集起来，按「谁来画」分给三套字号。
 *
 * 手动逐条列的话，新加一个字段（比如刚才加的 menu_sub）很容易忘了加进
 * 检查表 —— 那时缺字就查不出来。所以这里按结构体字段遍历，而不是按
 * 「我记住了哪几个」。
 */
#define BAG_BOX_MAX 16
#define BAG_DLG_MAX 48

static int check_screen_charset(const BattleScreen *bs, const MenuScreen *ms)
{
    const char *box[BAG_BOX_MAX];
    const char *dlg[BAG_DLG_MAX];
    int nb = 0, nd = 0;

    if (bs) {
        /* 12px：血条框里的名字 / 等级 / 血量 */
        box[nb++] = bs->me_name;
        box[nb++] = bs->foe_name;
        box[nb++] = bs->me_lv;
        box[nb++] = bs->foe_lv;
        box[nb++] = bs->me_hp_text;
        /* 16px：日志、招式菜单、状态行 */
        dlg[nd++] = bs->log[0];
        dlg[nd++] = bs->log[1];
        for (int i = 0; i < 4; i++) {
            dlg[nd++] = bs->moves[i];
            dlg[nd++] = bs->move_pp[i];
        }
        dlg[nd++] = bs->status_left;
        dlg[nd++] = bs->status_right;
    }
    if (ms) {
        box[nb++] = ms->title;    /* 弹层的标题用 12px 画（框内一行） */
        dlg[nd++] = ms->title;    /* 整屏菜单的标题用 16px 画（标题横条） */
        dlg[nd++] = ms->big;
        dlg[nd++] = ms->sub;
        dlg[nd++] = ms->status_left;
        dlg[nd++] = ms->status_right;
        int n = ms->rows;
        if (n > UI_MENU_ROWS_MAX) {
            n = UI_MENU_ROWS_MAX;
        }
        for (int i = 0; i < n; i++) {
            dlg[nd++] = ms->items[i];
            dlg[nd++] = ms->notes[i];
        }
    }

    int bad = 0;
    bad += check_charset(&kFontApp12, "血条框/弹层标题(12px)", box, nb);
    bad += check_charset(&kFontApp16, "对话框/列表(16px)", dlg, nd);
    return bad;
}

/* ================================================================ 会话试验台 */

/*
 * 与 test_session.c 里的 Rig 同构，但这里只用来**走到某一屏**，不做断言。
 * 单独写一份而不共享，是因为共享要新建一个 .h/.c 放进 src/ —— 那是固件里
 * 不该有的东西（固件不会自己按按键走位）。二十行代码，不值得。
 */
typedef struct {
    Session  s;
    uint32_t t;
} Rig;

static const UiFonts kFonts = {
    .box    = &kFontApp12,
    .dialog = &kFontApp16,
    .label  = &kFontHp10,
};

static void rig_init(Rig *r)
{
    rng_reset();
    ses_init(&r->s, AI_ROOKIE);   /* 不做搜索，跑得快 */
    ses_set_fonts(&r->s, &kFonts);
    r->t = 0;
}

static void ev(Rig *r, KeyId k, KeyEvent e)
{
    r->t += 16;
    KeyMsg m = { .key = k, .ev = e, .mv = 0, .t_ms = r->t };
    ses_feed(&r->s, &m, r->t);
}

static void tap(Rig *r, KeyId k)
{
    ev(r, k, KEY_EV_PRESS);
    ev(r, k, KEY_EV_CLICK);
    ev(r, k, KEY_EV_RELEASE);
}

static void hold(Rig *r, KeyId k)
{
    ev(r, k, KEY_EV_PRESS);
    ev(r, k, KEY_EV_LONG);
    ev(r, k, KEY_EV_RELEASE);
}

static void ok_(Rig *r)   { tap(r, KEY_OK); }
static void down_(Rig *r) { tap(r, KEY_DOWN); }

static void play_log(Rig *r)
{
    int n = 0;
    while (ses_phase(&r->s) == SES_PHASE_LOG && n < 400) {
        ok_(r);
        n++;
    }
}

/* 从主菜单开局并放完开场日志 */
static void start_battle(Rig *r)
{
    down_(r);   /* 光标从第 1 项「选择队伍」移到第 2 项「开始对战」 */
    ok_(r);
    play_log(r);
}

/* ================================================================ 各屏数据 */

/* 画一屏：按会话给的分层决定画几层 */
static void draw(Surface *s, SesLayer layer, const BattleScreen *bs,
                 const MenuScreen *ms)
{
    switch (layer) {
    case SES_LAYER_MENU:
        ui_menu_screen(s, &kFonts, ms);
        break;
    case SES_LAYER_BATTLE:
        ui_battle_screen(s, &kFonts, bs);
        break;
    case SES_LAYER_BATTLE_MENU:
        ui_battle_screen(s, &kFonts, bs);
        ui_overlay_screen(s, &kFonts, ms);
        break;
    default:
        break;
    }
}

/*
 * 手工摆的 hero 数据。数值与 make_readme_images.py 的 make_hero() 一一对应，
 * 这样两张图才能逐块对照。
 */
static int build_hero(BattleScreen *d)
{
    static const char *const foe_name = "巨金怪";
    static const char *const foe_lv = "Lv50";
    static const char *const me_name = "喷火龙";
    static const char *const me_lv = "Lv50";
    static const char *const me_hp = "68/153";
    static const char *const log0 = "喷火龙 使用了 喷射火焰！";
    static const char *const log1 = "会心一击！造成了 187 点伤害";
    static const char *const mv[4] = { "喷射火焰", "龙爪", "翅膀攻击", "剑舞" };
    static const char *const pp[4] = { "15", "15", "35", "20" };

    memset(d, 0, sizeof(*d));
    d->foe_sprite = sprite_get(376, SPR_FRONT);   /* metagross */
    d->foe_name = foe_name;
    d->foe_lv = foe_lv;
    d->foe_hp_permil = 720;

    d->me_sprite = sprite_get(6, SPR_FRONT);      /* charizard */
    d->me_name = me_name;
    d->me_lv = me_lv;
    d->me_hp_permil = 440;
    d->me_hp_text = me_hp;

    d->log[0] = log0;
    d->log[1] = log1;
    for (int i = 0; i < 4; i++) {
        d->moves[i] = mv[i];
        d->move_pp[i] = pp[i];
    }
    d->selected = 0;

    /* 状态行右侧用**状态机里那条真实提示**，不再写一句手造的
     * 「选择招式   换人   认输」—— 那句话在固件里不存在，图却在 README 上
     * 挂着，看的人会以为界面上真有那么一行。 */
    d->status_left = "PP 15/15";
    d->status_right = "选择招式   长按换人";

    return (d->foe_sprite && d->me_sprite) ? 0 : -1;
}

/*
 * 走到指定的一屏。返回会话给的分层；**到不了就报错退出**，不导出一张
 * 名不副实的图 —— 「导出的图不对」比「导出失败」更难发现。
 */
static SesLayer prep(const char *name, Rig *r, BattleScreen *bs, MenuScreen *ms,
                     bool *has_menu)
{
    rig_init(r);

    if (strcmp(name, "main") == 0) {
        /* 开局就在主菜单，什么都不用按 */
    } else if (strcmp(name, "preset") == 0) {
        ok_(r);                    /* 主菜单第 1 项「选择队伍」→ 五支预设队列表 */
    } else if (strcmp(name, "team") == 0) {
        /* 主菜单第 3 项才是「队伍配置」：▼ 移光标（作用在 PRESS 上），
         * 再确定进入。**只按 ▼ 不会进界面** —— 光标移动不改变当前屏。 */
        down_(r);
        down_(r);
        ok_(r);
    } else if (strcmp(name, "log") == 0) {
        down_(r);                  /* 移到第 2 项「开始对战」 */
        ok_(r);                    /* 开始对战 → 开场日志，只放到第一条 */
    } else if (strcmp(name, "action") == 0) {
        start_battle(r);
        hold(r, KEY_OK);           /* 战斗界面上长按 = 打开弹层 */
    } else if (strcmp(name, "confirm") == 0) {
        start_battle(r);
        hold(r, KEY_OK);           /* 弹层，光标停在「换人」 */
        down_(r);                  /* 移到「认输」 */
        ok_(r);                    /* 进二次确认 */
    } else if (strcmp(name, "switch") == 0) {
        start_battle(r);
        hold(r, KEY_OK);           /* 弹层，光标停在「换人」 */
        ok_(r);                    /* 进换人列表 */
    } else if (strcmp(name, "result") == 0) {
        start_battle(r);
        int n = 0;
        while (ses_phase(&r->s) != SES_PHASE_RESULT && n < 20000) {
            ok_(r);                /* 一路确定（战斗界面上确定 = 出第 1 招） */
            n++;
        }
    } else {
        fprintf(stderr, "不认识的界面：%s（--list 看可选项）\n", name);
        exit(2);
    }

    SesLayer layer = ses_layer(&r->s);
    ses_view(&r->s, bs);
    *has_menu = ses_menu_view(&r->s, ms);

    /* 校验：确实走到了预期的那一屏 */
    UiScreen want, got = ui_state_screen(&r->s.ui);
    if (strcmp(name, "main") == 0)        { want = UI_SCREEN_MAIN; }
    else if (strcmp(name, "preset") == 0) { want = UI_SCREEN_PRESET; }
    else if (strcmp(name, "team") == 0)   { want = UI_SCREEN_TEAM; }
    else if (strcmp(name, "log") == 0)    { want = UI_SCREEN_BATTLE; }
    else if (strcmp(name, "action") == 0) { want = UI_SCREEN_ACTION; }
    else if (strcmp(name, "confirm") == 0){ want = UI_SCREEN_CONFIRM; }
    else if (strcmp(name, "switch") == 0) { want = UI_SCREEN_SWITCH; }
    else                                  { want = UI_SCREEN_RESULT; }

    if (got != want) {
        fprintf(stderr, "挑了 %s，结果走到的是界面 %d 而不是 %d\n",
                name, (int)got, (int)want);
        exit(2);
    }
    /* 菜单类界面必须真的拿到了菜单数据，否则图上是空的 */
    if (layer != SES_LAYER_BATTLE && !*has_menu) {
        fprintf(stderr, "%s：会话说该画菜单，但 ses_menu_view() 没给数据\n", name);
        exit(2);
    }
    return layer;
}

/* ================================================================ main */

static const char *const kScreens[] = {
    "main", "preset", "team", "log", "battle", "action", "confirm", "switch",
    "result",
};
#define SCREEN_COUNT ((int)(sizeof(kScreens) / sizeof(kScreens[0])))

static int dump_one(const char *name, const char *out_dir)
{
    BattleScreen bs;
    MenuScreen   ms;
    SesLayer     layer = SES_LAYER_BATTLE;
    bool         has_menu = false;
    Rig          r;

    memset(&bs, 0, sizeof(bs));
    memset(&ms, 0, sizeof(ms));

    if (strcmp(name, "battle") == 0) {
        if (build_hero(&bs) != 0) {
            fprintf(stderr, "精灵图缺失（pid 376 或 6）\n");
            return 1;
        }
    } else {
        layer = prep(name, &r, &bs, &ms, &has_menu);
    }

    int bad = check_screen_charset(&bs, has_menu ? &ms : NULL);
    if (bad) {
        fprintf(stderr, "共 %d 处缺字，先跑 `make font` / `make lvgl-font` 补字库再出图\n",
                bad);
        return 1;
    }

    Surface s;
    r_init(&s, g_fb, W, H, W);
    r_fill(&s, 0);   /* 先涂黑：万一有没盖到的地方，图上一眼能看出来 */
    draw(&s, layer, &bs, &ms);

    char path[512];
    snprintf(path, sizeof(path), "%s/%s.ppm", out_dir, name);
    write_ppm(path, g_fb, W, H);

    printf("  已导出 %-28s（%dx%d，%s）\n", path, W, H,
           layer == SES_LAYER_MENU ? "整屏菜单"
           : layer == SES_LAYER_BATTLE ? "战斗界面" : "战斗 + 弹层");
    return 0;
}

int main(int argc, char **argv)
{
    const char *screen = "battle";
    const char *out_dir = "data/screen_preview";
    const char *out_file = NULL;
    bool all = false;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--out") == 0 && i + 1 < argc) {
            out_file = argv[++i];
        } else if (strcmp(argv[i], "--screen") == 0 && i + 1 < argc) {
            screen = argv[++i];
        } else if (strcmp(argv[i], "--dir") == 0 && i + 1 < argc) {
            out_dir = argv[++i];
        } else if (strcmp(argv[i], "--all") == 0) {
            all = true;
        } else if (strcmp(argv[i], "--list") == 0) {
            for (int k = 0; k < SCREEN_COUNT; k++) {
                printf("%s\n", kScreens[k]);
            }
            return 0;
        } else {
            fprintf(stderr, "未知参数 %s\n", argv[i]);
            return 2;
        }
    }

    printf("导出界面：%d 屏\n", all ? SCREEN_COUNT : 1);
    printf("  字库：12px %u 字形 / 16px %u 字形 / 10px %u 字形\n",
           kFontApp12.glyph_count, kFontApp16.glyph_count,
           kFontHp10.glyph_count);

    int rc = 0;
    if (all) {
        for (int k = 0; k < SCREEN_COUNT; k++) {
            rc |= dump_one(kScreens[k], out_dir);
        }
    } else {
        /* --out 只对单屏有意义；--all 时每屏一个文件名 */
        rc = dump_one(screen, out_dir);
        if (rc == 0 && out_file) {
            char from[512];
            snprintf(from, sizeof(from), "%s/%s.ppm", out_dir, screen);
            if (rename(from, out_file) != 0) {
                fprintf(stderr, "改名 %s -> %s 失败\n", from, out_file);
                rc = 1;
            } else {
                printf("  已改名为 %s\n", out_file);
            }
        }
    }
    return rc;
}
