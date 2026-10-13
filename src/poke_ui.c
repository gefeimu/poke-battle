/*
 * 战斗界面绘制实现。版式依据与设计理由见 poke_ui.h。
 */
#include "poke_ui.h"

#include <stddef.h>

/* ================================================================ 配色 */
/*
 * 全部取自三代火红/叶绿的原始调色板文件，不是随手挑的。
 * 后面括号里是索引，便于回查。
 */
#define C_HB_FILL       RGB565_C(255, 255, 222)   /* healthbox.pal[2]  米白填充 */
#define C_HB_EDGE       RGB565_C(65, 65, 65)      /* healthbox.pal[1]  深灰描边 */
#define C_HB_TEXT       RGB565_C(32, 57, 0)       /* healthbox.pal[7]  框内文字（深绿） */
#define C_HB_SHADOW     RGB565_C(82, 106, 98)     /* healthbox.pal[6]  右下投影（墨绿） */

#define C_BAR_TRACK     RGB565_C(57, 57, 57)      /* healthbar.pal     条槽 */

/* HP 条是**双色立体**：上行高光 + 下行基色。单色画会明显「不像」。 */
#define C_BAR_GREEN_HI  RGB565_C(115, 255, 172)
#define C_BAR_GREEN_LO  RGB565_C(90, 213, 131)
#define C_BAR_YELLOW_HI RGB565_C(255, 230, 57)
#define C_BAR_YELLOW_LO RGB565_C(205, 172, 8)
#define C_BAR_RED_HI    RGB565_C(255, 90, 57)
#define C_BAR_RED_LO    RGB565_C(172, 65, 74)

#define C_HP_LABEL      RGB565_C(255, 230, 0)     /* healthbox.pal[9] 框内唯一的黄 */
#define C_HP_LABEL_EDGE RGB565_C(0, 0, 0)         /* 标签黑描边 */

#define C_DLG_FILL      RGB565_C(255, 255, 255)   /* textbox1.pal      白底 */
#define C_DLG_EDGE      RGB565_C(65, 74, 123)     /* 深蓝外框 */
#define C_DLG_EDGE_HI   RGB565_C(106, 139, 189)   /* 浅蓝内框 */
#define C_DLG_TEXT      RGB565_C(41, 49, 49)
#define C_DLG_GOLD      RGB565_C(156, 123, 24)

#define C_SEL_FILL      RGB565_C(255, 230, 57)
#define C_SEL_EDGE      RGB565_C(205, 172, 8)

#define C_SKY_TOP       RGB565_C(108, 176, 232)
#define C_SKY_BOT       RGB565_C(196, 226, 248)
#define C_GROUND        RGB565_C(152, 200, 112)
#define C_GROUND_DARK   RGB565_C(120, 176, 88)
#define C_PLATFORM      RGB565_C(184, 220, 148)
#define C_PLATFORM_DARK RGB565_C(144, 192, 108)

/* ================================================================ 版式 */
/* 血条框：尺寸来自原版 OAM（64x64 两片拼合 = 128 宽） */
#define HB_RADIUS       5
#define HB_INSET        2      /* 离屏幕边 2px 缝隙（本项目调整，原版贴边） */
#define HB_FOE_W        128
#define HB_FOE_H        32
#define HB_ME_W         128
#define HB_ME_H         44     /* 原版 64：最下面压着 EXP 条，本项目不用，压到 44 */
#define HB_NAME_DY      1
#define HB_HP_DY        16
#define HB_NUM_DY       28     /* 仅我方框有数字行 */
#define HP_ROW_H        10     /* 含嵌入的 HP 标签；彩色填充实占 6px */
#define HP_LABEL_GAP    6      /* 标签右边到填充的间距 */

/* 场景 */
#define HORIZON         84
#define FOE_X           146
#define FOE_Y           22
#define ME_X            10
#define ME_Y            70

/* 下半屏 */
#define DLG_X           4
#define DLG_Y           164
#define DLG_W           232
#define DLG_H           52
#define MOVE_X          4
#define MOVE_Y          220
#define MOVE_W          232
#define MOVE_H          64
#define MOVE_CELL_W     106
#define MOVE_CELL_H     24
#define MOVE_COL_STEP   112
#define MOVE_ROW_STEP   28
#define MOVE_COL0       12
#define MOVE_ROW0       225
#define STATUS_X        4
#define STATUS_Y        288
#define STATUS_W        232
#define STATUS_H        28
/* 注意这是**绝对** x（不是相对框的偏移）—— 与 make_readme_images.py 一致。
 * 曾写成相对偏移，结果文字比 PC 版右移 4px，图对比时露馅。 */
#define STATUS_TEXT_X   100

/* ================================================================ 内部 */

/*
 * 字形墨迹顶边相对 r_text() 那个 y 的偏移。
 *
 * 公式来自 r_text 的内部逻辑：墨迹顶边
 *   = baseline - ofs_y - box_h
 *   = (y + line_height - base_line) - ofs_y - box_h
 *
 * **不要写死这个数**：换字号之后它就变了（12px 是 3、10px 是 1），
 * 写死会让标签在胶囊里偏上或偏下。Python 版有同名函数。
 */
static int ink_top(const BitmapFont *f, uint32_t cp)
{
    if (!f) {
        return 0;
    }
    const GlyphDsc *g = font_glyph(f, font_glyph_index(f, cp));
    if (!g) {
        return 0;
    }
    return f->line_height - f->base_line - g->ofs_y - g->box_h;
}

/* 精灵图**可见像素**的下边界（即最后一行墨迹的下一个 y，与 PIL 的 bbox lower 一致）。
 *
 * 不能拿 h（64）当脚底：立绘画布底部通常留一圈透明（巨金怪留了 6px），
 * 按画布摆站台会让它悬在脚底下。这是看预览图之后才发现的。 */
static int sprite_ink_bottom(const SpriteEntry *spr)
{
    if (!spr || !spr->data) {
        return 0;
    }
    for (int y = spr->h - 1; y >= 0; y--) {
        for (int x = 0; x < spr->w; x++) {
            int i = y * spr->w + x;
            uint8_t idx = (uint8_t)((spr->data[i / 2] >> ((i % 2) ? 0 : 4)) & 0x0F);
            if (idx != PAL_TRANSPARENT) {
                return y + 1;
            }
        }
    }
    return spr->h;
}

static void hp_colors(int ratio_permil, uint16_t *hi, uint16_t *lo)
{
    if (ratio_permil > 500) {
        *hi = C_BAR_GREEN_HI;
        *lo = C_BAR_GREEN_LO;
    } else if (ratio_permil > 200) {
        *hi = C_BAR_YELLOW_HI;
        *lo = C_BAR_YELLOW_LO;
    } else {
        *hi = C_BAR_RED_HI;
        *lo = C_BAR_RED_LO;
    }
}

/*
 * HP 条 —— **「HP」标签嵌在条的左端，整体是一个元素**（原版即如此）。
 *
 *     [ 深色胶囊 .......................... ]
 *      └ 左端：金色「HP」（黑描边）
 *      └ 其后：彩色填充，上下各内缩 2px
 *
 * 曾经画成「左边一个 HP 文字 + 右边一条独立的条」，看着散、横向还多占一个
 * 标签的宽度。这类「元素是不是一体的」判断光看文字描述推不出来，
 * 是看了原版截图才改对的。
 */
static void hp_row(Surface *s, const UiFonts *f, int x, int y, int w, int h,
                   int ratio_permil, bool label)
{
    r_round_rect(s, x, y, w, h, h / 2, C_BAR_TRACK);

    int fx = x + 2;
    if (label && f && f->label) {
        const GlyphDsc *hg = font_glyph(f->label,
                                        font_glyph_index(f->label, 'H'));
        int box_h = hg ? hg->box_h : h - 4;
        int dy = y + (h - box_h) / 2 - ink_top(f->label, 'H');
        r_text_outlined(s, x + 4, dy, f->label, "HP",
                        C_HP_LABEL, C_HP_LABEL_EDGE);
        fx = x + 4 + r_text_width(f->label, "HP") + HP_LABEL_GAP;
    }

    int inner_w = (x + w - 4) - fx;
    if (inner_w < 5) {
        return;
    }
    int fill_w = (inner_w * ratio_permil + 500) / 1000;
    if (fill_w > inner_w) {
        fill_w = inner_w;
    }
    if (fill_w < 5) {
        return;
    }

    uint16_t hi, lo;
    hp_colors(ratio_permil, &hi, &lo);

    r_round_rect(s, fx, y + 2, fill_w, h - 4, (h - 4) / 2, lo);
    if (fill_w > 7) {
        /* 高光占填充高度的上半（原版是上下各一半） */
        int hi_h = (h - 4 + 1) / 2;
        if (hi_h < 1) {
            hi_h = 1;
        }
        r_fill_rect(s, fx + 2, y + 2, fill_w - 4, hi_h, hi);
    }
}

/* ================================================================ 对外 */

void ui_dialog(Surface *s, int x, int y, int w, int h, int radius)
{
    r_round_rect(s, x, y, w, h, radius, C_DLG_EDGE);
    r_round_rect(s, x + 3, y + 3, w - 6, h - 6,
                 radius > 2 ? radius - 2 : 0, C_DLG_FILL);
    /* 内框线：浅蓝，贴在填充里侧 1px */
    r_round_rect_outline(s, x + 3, y + 3, w - 6, h - 6,
                         radius > 2 ? radius - 2 : 0,
                         C_DLG_EDGE_HI, 1, R_NO_FILL);
}

void ui_healthbox(Surface *s, const UiFonts *f, int x, int y, int w, int h,
                  const char *name, const char *lv, int ratio_permil,
                  const char *hp_text)
{
    /* 右下投影。注意这一层会超到框外右下方 3px —— 放在屏幕右下角时
     * 会有一列被裁掉，这与原版行为一致（原版的投影也是出界的）。 */
    r_round_rect(s, x + 3, y + 3, w, h, HB_RADIUS, C_HB_SHADOW);
    /* 描边 + 填充 */
    r_round_rect(s, x, y, w, h, HB_RADIUS, C_HB_EDGE);
    r_round_rect(s, x + 2, y + 2, w - 4, h - 4, HB_RADIUS - 2, C_HB_FILL);

    if (f && f->box) {
        r_text(s, x + 8, y + HB_NAME_DY, f->box, name, C_HB_TEXT);
        if (lv) {
            int lv_x = x + w - 8 - r_text_width(f->box, lv);
            r_text(s, lv_x, y + HB_NAME_DY, f->box, lv, C_HB_TEXT);
        }
    }

    hp_row(s, f, x + 8, y + HB_HP_DY, w - 16, HP_ROW_H, ratio_permil, true);

    if (hp_text && f && f->box) {
        int tx = x + w - 8 - r_text_width(f->box, hp_text);
        r_text(s, tx, y + HB_NUM_DY, f->box, hp_text, C_HB_TEXT);
    }
}

void ui_battle_screen(Surface *s, const UiFonts *f, const BattleScreen *d)
{
    if (!s || !f || !d) {
        return;
    }

    /* ---- 场景：天空 + 地面，铺满整屏（对话框浮在上面）----
     * 原版画面只有 240x160，我们屏高一倍，所以场景铺满整屏，
     * 比「下半屏另起一块纯色面板」更接近原版观感。 */
    r_grad_v(s, 0, 0, UI_SCREEN_W, HORIZON, C_SKY_TOP, C_SKY_BOT);
    r_grad_v(s, 0, HORIZON, UI_SCREEN_W, UI_SCREEN_H - HORIZON,
             C_GROUND, C_GROUND_DARK);

    /* ---- 立绘与站台 ---- */
    int foe_feet = FOE_Y + sprite_ink_bottom(d->foe_sprite);
    int me_feet = ME_Y + sprite_ink_bottom(d->me_sprite);

    /* 站台画在立绘之前，下缘会被立绘压住一点，看起来才是「站在上面」 */
    r_ellipse(s, 178, foe_feet + 2, 40, 9, C_PLATFORM_DARK);
    r_ellipse(s, 178, foe_feet, 37, 9, C_PLATFORM);
    r_ellipse(s, 42, me_feet + 2, 44, 10, C_PLATFORM_DARK);
    r_ellipse(s, 42, me_feet, 41, 10, C_PLATFORM);

    if (d->foe_sprite) {
        r_sprite(s, FOE_X, FOE_Y, d->foe_sprite);
    }
    if (d->me_sprite) {
        r_sprite(s, ME_X, ME_Y, d->me_sprite);
    }

    /* ---- 两个血条框：对角摆放，离屏幕边留 2px ---- */
    ui_healthbox(s, f, HB_INSET, HB_INSET, HB_FOE_W, HB_FOE_H,
                 d->foe_name, d->foe_lv, d->foe_hp_permil, NULL);
    ui_healthbox(s, f, UI_SCREEN_W - HB_ME_W - HB_INSET, 110, HB_ME_W, HB_ME_H,
                 d->me_name, d->me_lv, d->me_hp_permil, d->me_hp_text);

    /* ---- 对话框（战斗日志）---- */
    ui_dialog(s, DLG_X, DLG_Y, DLG_W, DLG_H, 6);
    if (f->dialog) {
        if (d->log[0]) {
            r_text(s, DLG_X + 10, DLG_Y + 4, f->dialog, d->log[0], C_DLG_TEXT);
        }
        if (d->log[1]) {
            r_text(s, DLG_X + 10, DLG_Y + 25, f->dialog, d->log[1], C_DLG_TEXT);
        }
    }

    /* ---- 招式菜单 2x2 ---- */
    ui_dialog(s, MOVE_X, MOVE_Y, MOVE_W, MOVE_H, 6);
    for (int i = 0; i < 4; i++) {
        int col = i % 2;
        int row = i / 2;
        int mx = MOVE_COL0 + col * MOVE_COL_STEP;
        int my = MOVE_ROW0 + row * MOVE_ROW_STEP;
        bool sel = (i == d->selected);

        if (sel) {
            r_round_rect(s, mx, my, MOVE_CELL_W, MOVE_CELL_H, 3, C_SEL_EDGE);
            r_round_rect(s, mx + 2, my + 2, MOVE_CELL_W - 4, MOVE_CELL_H - 4,
                         2, C_SEL_FILL);
            /* 光标三角 */
            for (int k = 0; k < 4; k++) {
                r_vline(s, mx + 6 + k, my + 8 + k, 9 - 2 * k, C_DLG_TEXT);
            }
        } else {
            r_round_rect_outline(s, mx, my, MOVE_CELL_W, MOVE_CELL_H, 3,
                                 C_DLG_EDGE_HI, 1, R_NO_FILL);
        }

        if (!f->dialog) {
            continue;
        }
        uint16_t ink = sel ? C_DLG_TEXT : C_DLG_TEXT;
        if (d->moves[i]) {
            r_text(s, mx + 14, my + 2, f->dialog, d->moves[i], ink);
        }
        if (d->move_pp[i]) {
            int pw = r_text_width(f->dialog, d->move_pp[i]);
            r_text(s, mx + MOVE_CELL_W - 4 - pw, my + 2, f->dialog,
                   d->move_pp[i], sel ? C_DLG_GOLD : C_DLG_TEXT);
        }
    }

    /* ---- 状态行 ---- */
    ui_dialog(s, STATUS_X, STATUS_Y, STATUS_W, STATUS_H, 5);
    if (f->dialog) {
        if (d->status_left) {
            r_text(s, STATUS_X + 10, STATUS_Y + 4, f->dialog,
                   d->status_left, C_DLG_GOLD);
        }
        if (d->status_right) {
            r_text(s, STATUS_TEXT_X, STATUS_Y + 4, f->dialog,
                   d->status_right, C_DLG_TEXT);
        }
    }
}
