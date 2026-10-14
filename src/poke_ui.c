/*
 * 战斗界面绘制实现。版式依据与设计理由见 poke_ui.h。
 */
#include "poke_ui.h"

#include <stddef.h>

/* ================================================================ 配色 */
/*
 * 战斗界面的配色全部取自三代火红/叶绿的原始调色板文件，不是随手挑的。
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

/*
 * ---- 菜单配色（本项目自定，没有原版可抄：原版没有竖屏菜单）----
 *
 * 沿用战斗界面那套「蓝框白底」的视觉语言，只补三个新颜色：
 * 标题横条的深蓝、滚动条的浅灰轨道、结算大字的胜负色。
 */
#define C_WHITE         RGB565_C(255, 255, 255)
#define C_TITLE_FILL    RGB565_C(41, 74, 123)     /* 与 C_DLG_EDGE 同族，深一档 */
#define C_TITLE_EDGE    RGB565_C(20, 40, 78)
#define C_BAR_TRACK_LT  RGB565_C(214, 226, 238)   /* 白底上的滚动条轨道 */
#define C_BAR_FILL      RGB565_C(65, 74, 123)
#define C_NOTE_TEXT     RGB565_C(96, 106, 118)    /* 行右侧小字（未选中时） */

/* 结算大字：胜红败蓝。与股市那套红涨绿跌无关，纯粹是「红=庆祝」的直觉。 */
#define C_WIN_FILL      RGB565_C(214, 60, 52)
#define C_WIN_EDGE      RGB565_C(150, 30, 26)
#define C_LOSE_FILL     RGB565_C(58, 96, 168)
#define C_LOSE_EDGE     RGB565_C(30, 56, 110)
#define C_DRAW_FILL     RGB565_C(120, 126, 134)
#define C_DRAW_EDGE     RGB565_C(78, 84, 92)

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
#define HB_ME_X         (UI_SCREEN_W - HB_ME_W - HB_INSET)
#define HB_ME_Y         110    /* 我方框贴下半屏上方；提出常量只为去字面量 */

/* 我方携带道具名：画在血条框 HP 数字行的**左侧**（数字是右对齐的，
 * 两者不会撞在一起），用血条框字号（12px）。 */
#define MY_ITEM_X       (HB_ME_X + 8)
#define MY_ITEM_DY      HB_NUM_DY

/* 场景 */
#define HORIZON         84
#define FOE_X           146
#define FOE_Y           22
#define ME_X            10
#define ME_Y            70

/*
 * 天气胶囊：画在**画面右上角**并右对齐（不是水平居中）——
 * 左上角被对手血条框（y ≤ 34）占着，右上角对手立绘从 y=22 起，
 * y 4..20 这一条正好是空的，放这里不压任何东西。
 */
#define WX_BADGE_Y      4
#define WX_BADGE_H      16
#define WX_BADGE_PAD_X  6
#define WX_BADGE_R      5
#define WX_BADGE_RIGHT  4

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

/*
 * 两个文字宽度预算（定义在 poke_ui.h，绘制与折行共用）必须与这里的版式
 * 对得上。写成编译期断言：以后有人改 DLG_W / STATUS_TEXT_X 却忘了同步
 * 折行宽度，会直接编译不过，而不是画出一行被裁掉的日志。
 *
 *   对话框文字：左右各留 10px 内边距
 *   状态行提示：从 STATUZ_TEXT_X 到框右边再留 10px
 */
_Static_assert(UI_DLG_TEXT_W == DLG_W - 20,
               "UI_DLG_TEXT_W 与对话框内边距不一致");
_Static_assert(UI_STATUS_HINT_W == STATUS_W - (STATUS_TEXT_X - STATUS_X) - 10,
               "UI_STATUS_HINT_W 与状态行版式不一致");

/* ============================================================ 菜单版式 */
/*
 * 菜单版式（本项目自定，理由见 poke_ui.h 的「版式依据」）。
 * 所有数字都收在这一段，下面的绘制函数只引用名字、不出现字面量 ——
 * 以后调版式只改这里，不用满文件找魔法数。
 *
 * 竖轴的分区（320 高）：
 *
 *     6 .. 38    标题横条        32 高
 *    44 .. 280   列表板          最高 8 行 × 28 + 上下各 6 = 236
 *   288 .. 316   状态行          与战斗界面同一处，开弹层时不跳位
 */
#define MENU_TITLE_X     4
#define MENU_TITLE_Y     6
#define MENU_TITLE_W     (UI_SCREEN_W - 8)     /* 232 */
#define MENU_TITLE_H     32
#define MENU_TITLE_R     5

#define MENU_BOX_X       4
#define MENU_BOX_Y       44
#define MENU_BOX_W       (UI_SCREEN_W - 8)     /* 232 */
#define MENU_BOX_R       6
#define MENU_BOX_PAD_Y   6                     /* 首行离板内顶；板高 = 上下各一个 */

#define MENU_ROW_H       28
#define MENU_HL_INSET    5                     /* 高亮块离板边 */
#define MENU_HL_H        (MENU_ROW_H - 2)      /* 上下各让 1px，行与行之间才有缝 */
#define MENU_TEXT_DX     14                    /* 行文字 x 相对板左边（选中与否都一样，
                                                * 否则选中时文字会横向跳一下） */
#define MENU_NOTE_R      20                    /* 行右侧小字离板右边 */
#define MENU_TRI_DX      6                     /* 光标三角 x 相对板左边 */

/* ---- 带图标的行（队伍配置 / 选择队伍 / 换人）----
 * 全部按「板内相对坐标」定义，与 MENU_TEXT_DX / MENU_NOTE_R 同一坐标系。
 * 行高 37 时：图标 32 高上下各留 2、名字在第一行、等级/HP 数字在第二行。 */
#define MENU_ICON_DX       8     /* 行首图标离板左边（绝对 12，图标 12..44） */
#define MENU_ICON_DY       2     /* 图标在行高 37 里竖直居中：(37-32)/2 */
#define MENU_ICON_TEXT_DX  46    /* 有图标的行，文字右移到这里（绝对 50） */
#define MENU_SUB_DY        20    /* 第二行（等级 / cur/max）相对行顶的偏移 */
#define MENU_HPBAR_X       126   /* HP 条离板左边（绝对 130；板宽 232，条 130..204） */
#define MENU_HPBAR_W       74
#define MENU_HPBAR_H       8
#define MENU_HPBAR_R       4
#define MENU_HPBAR_DY      4     /* HP 条相对行顶（y+4..12），与第二行 y+20 错开 */

#define MENU_BAR_X       (MENU_BOX_X + MENU_BOX_W - 9)
#define MENU_BAR_W       5
#define MENU_BAR_PAD     5
/* 386 项时按比例算出来的滑块只有 0.6px，四舍五入成 0 就整条不见了。
 * 给个下限，宁可滑块偏大也不能消失 —— 滚动条一消失，用户就以为
 * 「列表到底了」。 */
#define MENU_BAR_MIN_H   10

#define MENU_BIG_X       MENU_BOX_X
#define MENU_BIG_W       MENU_BOX_W
#define MENU_BIG_Y       120
#define MENU_BIG_H       84
#define MENU_BIG_PAD     14                    /* 大字离板内顶 */
#define MENU_SUB_GAP     30                    /* 大字到小字的行距 */

/* 弹层：**正好复用招式菜单的矩形**（打开弹层就把它盖住） */
#define OVL_X            MOVE_X
#define OVL_Y            MOVE_Y
#define OVL_W            MOVE_W
#define OVL_H            MOVE_H
#define OVL_R            6
/* 框内一行小字标题 + 两行选项。64 = 16 + 2×24，正好把招式框填满。 */
#define OVL_HDR_H        16
#define OVL_ROW_H        24
#define OVL_HL_H         (OVL_ROW_H - 2)
#define OVL_NOTE_R       12
#define OVL_TITLE_DX     10
#define OVL_MODAL_PAD    2                     /* 金色模态外框离框的距离 */

/* 列表板最高 8 行，不能压到状态行 */
_Static_assert(MENU_BOX_Y + MENU_BOX_PAD_Y * 2 + UI_MENU_ROWS_MAX * MENU_ROW_H
               <= STATUS_Y,
               "菜单列表板（最多 UI_MENU_ROWS_MAX 行）会压到状态行");
_Static_assert(MENU_TITLE_Y + MENU_TITLE_H + 6 <= MENU_BOX_Y,
               "标题横条与列表板重叠");
/* 弹层的两行 + 框内标题必须正好填满招式框，否则会露出一截招式菜单 */
_Static_assert(OVL_HDR_H + 2 * OVL_ROW_H == OVL_H,
               "弹层行高与招式菜单框高度对不上");

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

/*
 * 列表行里的小 HP 条。**不带「HP」标签**：换人列表一行只有 8px 高，
 * 塞不下标签，分档配色与战斗界面同一套（绿 >50% / 黄 >20% / 红）。
 * 填充画法与 hp_row() 完全一致 —— 两个条挨在同一屏上时观感才统一。
 */
static void menu_hpbar(Surface *s, int x, int y, int w, int h, int ratio_permil)
{
    uint16_t hi, lo;
    hp_colors(ratio_permil, &hi, &lo);
    r_round_rect(s, x, y, w, h, h / 2, C_BAR_TRACK);

    /* 无标签时 fx = x + 2（与 hp_row 的 fx 同一算法），右端留 4px */
    int inner_w = (x + w - 4) - (x + 2);
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

    r_round_rect(s, x + 2, y + 2, fill_w, h - 4, (h - 4) / 2, lo);
    if (fill_w > 7) {
        int hi_h = (h - 4 + 1) / 2;
        if (hi_h < 1) {
            hi_h = 1;
        }
        r_fill_rect(s, x + 2, y + 2, fill_w - 4, hi_h, hi);
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

/* 右上角天气胶囊：宽 = 文字宽 + 两倍内边距，整体右对齐贴边。 */
static void wx_badge(Surface *s, const UiFonts *f, const char *text)
{
    if (!text || !f || !f->dialog) {
        return;                 /* 无天气就不画：不占位、不遮天空 */
    }
    int tw = r_text_width(f->dialog, text);
    int w  = tw + WX_BADGE_PAD_X * 2;
    int x  = UI_SCREEN_W - WX_BADGE_RIGHT - w;
    r_round_rect(s, x, WX_BADGE_Y, w, WX_BADGE_H, WX_BADGE_R, C_HB_EDGE);
    r_round_rect(s, x + 1, WX_BADGE_Y + 1, w - 2, WX_BADGE_H - 2,
                 WX_BADGE_R - 1, C_HB_FILL);
    /* 文字竖直居中：16px 字库 line_height=21，居中会算出 ty = 4 + (16-21)/2 = 2，
     * 实际墨迹落在 6..18 之间，正好在胶囊 4..20 内。 */
    int ty = WX_BADGE_Y + (WX_BADGE_H - (int)f->dialog->line_height) / 2;
    r_text(s, x + WX_BADGE_PAD_X, ty, f->dialog, text, C_HB_TEXT);
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
    ui_healthbox(s, f, HB_ME_X, HB_ME_Y, HB_ME_W, HB_ME_H,
                 d->me_name, d->me_lv, d->me_hp_permil, d->me_hp_text);

    /* 我方携带道具名：HP 数字行左侧（数字右对齐，故不重叠）。无道具不画。 */
    if (d->my_item && f->box) {
        r_text(s, MY_ITEM_X, HB_ME_Y + MY_ITEM_DY, f->box, d->my_item,
               C_HB_TEXT);
    }

    /* 右上角天气标志：有天气才画。 */
    wx_badge(s, f, d->wx_text);

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

    /* 等玩家推进日志时的旋转动效。wait_anim 0 = 不画（自动推进中/非日志阶段）。 */
    if (d->wait_anim >= 1 && d->wait_anim <= 4 && f->dialog) {
        static const char *const kWaitFrames[4] = {"/", "-", "\\", "|"};
        r_text(s, STATUS_TEXT_X - 20, STATUS_Y + 4, f->dialog,
               kWaitFrames[d->wait_anim - 1], C_DLG_GOLD);
    }
}

/* ================================================================ 菜单 */

/*
 * 一屏列表的几何。整屏菜单与弹层都走这里，只是参数不同 ——
 * 「怎么排一行」只写一遍，两处就不会各排各的。
 */
typedef struct {
    int x;          /* 板左边 */
    int y;          /* **首行**的顶边（不是板顶） */
    int w;          /* 板宽（高亮块与文字都按它算） */
    int row_h;
    int hl_h;
    int note_r;     /* 右侧小字离板右边多少像素 */
} MenuBox;

/* 一行文字的行框 y 偏移：把 line_height 高的行框在 row_h 里竖直居中。
 * 不写死数字 —— 换字号（16px 是 21、12px 是 16）它就变了。 */
static int row_text_dy(const BitmapFont *f, int row_h)
{
    if (!f) {
        return 0;
    }
    int dy = (row_h - (int)f->line_height) / 2;
    return dy < 0 ? 0 : dy;
}

/* 窗口里实际能画几行：末尾不足一屏时按剩下的算，不然会画到 count 之外 */
static int menu_shown(const MenuScreen *m)
{
    if (!m || m->rows <= 0 || m->count <= 0) {
        return 0;
    }
    int shown = m->rows;
    if (m->top >= m->count) {
        return 0;
    }
    if (shown > m->count - m->top) {
        shown = m->count - m->top;
    }
    return shown;
}

/*
 * 画列表的行。**按窗口画，不按 count 画** —— 队伍配置是 386 项，
 * 照 count 循环会画 386 次、其中 378 次落在板外（裁剪只是不写内存，
 * 时间照样花）。嵌入式侧这个循环是每帧跑的。
 */
static void menu_list(Surface *s, const UiFonts *f, const MenuBox *b,
                      const MenuScreen *m)
{
    int shown = menu_shown(m);
    for (int i = 0; i < shown; i++) {
        int abs = m->top + i;          /* 绝对序号，用来和高亮比 */
        int y = b->y + i * b->row_h;
        bool sel = (abs == m->selected);

        if (sel) {
            r_round_rect(s, b->x + MENU_HL_INSET, y - 1,
                         b->w - MENU_HL_INSET * 2, b->hl_h, 3, C_SEL_EDGE);
            r_round_rect(s, b->x + MENU_HL_INSET + 2, y + 1,
                         b->w - MENU_HL_INSET * 2 - 4, b->hl_h - 4, 2,
                         C_SEL_FILL);
            /* 光标三角。与招式菜单同一画法（4 条竖线拼一个右向三角），
             * 竖直居中：行高可能是 28（整屏）也可能是 24（弹层）。 */
            int tri_y = y + (b->row_h - 9) / 2;
            if (tri_y < y) {
                tri_y = y;
            }
            for (int k = 0; k < 4; k++) {
                r_vline(s, b->x + MENU_TRI_DX + k, tri_y + k, 9 - 2 * k,
                        C_DLG_TEXT);
            }
        }

        /* 行首 32x32 图标：按原生尺寸绘制、不缩放，索引 0 透明不落笔。
         * 放在文字之前，且不受「有没有字体」影响（图标与字体无关）。 */
        if (m->row_icons[i]) {
            r_sprite(s, b->x + MENU_ICON_DX, y + MENU_ICON_DY,
                     m->row_icons[i]);
        }

        if (!f || !f->dialog) {
            continue;
        }
        int ty = y + row_text_dy(f->dialog, b->row_h);
        /* 选中与否文字 x **一样**：变了的话选中时整行文字会横跳一下，
         * 看着像画面抖了。只有有图标的行才右移让位。 */
        int text_dx = m->row_icons[i] ? MENU_ICON_TEXT_DX : MENU_TEXT_DX;
        if (m->items[i]) {
            r_text(s, b->x + text_dx, ty, f->dialog, m->items[i],
                   C_DLG_TEXT);
        }
        /* 第二行（换人行的等级）：与主文字同一 x，字号小一档。 */
        if (m->subs[i] && f->box) {
            r_text(s, b->x + text_dx, y + MENU_SUB_DY, f->box, m->subs[i],
                   C_DLG_TEXT);
        }
        /* HP 条：**> 0 才画**（哨兵 0 = 这个屏没有血条）。与战斗界面同一套分档。 */
        if (m->row_hp_permil[i] > 0) {
            menu_hpbar(s, b->x + MENU_HPBAR_X, y + MENU_HPBAR_DY,
                       MENU_HPBAR_W, MENU_HPBAR_H, m->row_hp_permil[i]);
        }
        if (m->notes[i]) {
            /* 有第二行的行（换人），cur/max 跟它同处第二行、同用 12px 字并
             * 右对齐；其余屏的 notes 仍落在主文字那一行（版式与从前一致）。 */
            bool on_sub = (m->subs[i] && f->box);
            const BitmapFont *nf = on_sub ? f->box : f->dialog;
            int ny = on_sub ? y + MENU_SUB_DY : ty;
            int nw = r_text_width(nf, m->notes[i]);
            r_text(s, b->x + b->w - b->note_r - nw, ny, nf,
                   m->notes[i], sel ? C_DLG_GOLD : C_NOTE_TEXT);
        }
    }
}

/*
 * 滚动条。只在「项数多于窗口」时画 —— 画一条永远满格的滚动条
 * 等于告诉用户「下面还有」，那是假信息。
 */
static void menu_scrollbar(Surface *s, const MenuScreen *m, int y, int h)
{
    if (!m->scrollbar || m->rows <= 0 || m->count <= m->rows || h <= 0) {
        return;
    }
    int x = MENU_BAR_X;
    r_round_rect(s, x, y, MENU_BAR_W, h, MENU_BAR_W / 2, C_BAR_TRACK_LT);

    int th = h * m->rows / m->count;
    if (th < MENU_BAR_MIN_H) {
        th = MENU_BAR_MIN_H;
    }
    if (th > h) {
        th = h;
    }
    int span = m->count - m->rows;     /* top 的合法范围是 0..span */
    int ty = y;
    if (span > 0) {
        ty = y + (h - th) * m->top / span;
    }
    if (ty + th > y + h) {
        ty = y + h - th;
    }
    r_round_rect(s, x, ty, MENU_BAR_W, th, MENU_BAR_W / 2, C_BAR_FILL);
}

/* 结算：一整块彩色板 + 正中的大字 */
static void menu_big(Surface *s, const UiFonts *f, const MenuScreen *m)
{
    uint16_t fill = C_DRAW_FILL, edge = C_DRAW_EDGE;
    switch (m->big_style) {
    case UI_BIG_WIN:  fill = C_WIN_FILL;  edge = C_WIN_EDGE;  break;
    case UI_BIG_LOSE: fill = C_LOSE_FILL; edge = C_LOSE_EDGE; break;
    case UI_BIG_DRAW:
    case UI_BIG_PLAIN:
    default:          break;
    }

    r_round_rect(s, MENU_BIG_X, MENU_BIG_Y, MENU_BIG_W, MENU_BIG_H,
                 MENU_BOX_R, edge);
    r_round_rect(s, MENU_BIG_X + 3, MENU_BIG_Y + 3,
                 MENU_BIG_W - 6, MENU_BIG_H - 6, MENU_BOX_R - 1, fill);

    if (f && f->dialog) {
        int tw = r_text_width(f->dialog, m->big);
        int tx = MENU_BIG_X + (MENU_BIG_W - tw) / 2;
        if (tx < MENU_BIG_X + 6) {
            tx = MENU_BIG_X + 6;
        }
        r_text_outlined(s, tx, MENU_BIG_Y + MENU_BIG_PAD, f->dialog, m->big,
                        C_WHITE, edge);
    }
    if (m->sub && f && f->box) {
        int sw = r_text_width(f->box, m->sub);
        int sx = MENU_BIG_X + (MENU_BIG_W - sw) / 2;
        if (sx < MENU_BIG_X + 6) {
            sx = MENU_BIG_X + 6;
        }
        r_text(s, sx, MENU_BIG_Y + MENU_SUB_GAP + MENU_BIG_PAD, f->box,
               m->sub, C_WHITE);
    }
}

void ui_overlay_rect(int *x, int *y, int *w, int *h)
{
    if (x) { *x = OVL_X; }
    if (y) { *y = OVL_Y; }
    if (w) { *w = OVL_W; }
    if (h) { *h = OVL_H; }
}

void ui_menu_screen(Surface *s, const UiFonts *f, const MenuScreen *m)
{
    if (!s || !f || !m) {
        return;
    }

    /* 背景铺满：**不依赖屏上原有的内容**，所以这一屏可以直接整屏重绘，
     * 也可以只重绘它自己那块裁剪区。 */
    r_grad_v(s, 0, 0, UI_SCREEN_W, UI_SCREEN_H, C_SKY_TOP, C_SKY_BOT);

    if (m->title && f->dialog) {
        r_round_rect(s, MENU_TITLE_X, MENU_TITLE_Y, MENU_TITLE_W,
                     MENU_TITLE_H, MENU_TITLE_R, C_TITLE_EDGE);
        r_round_rect(s, MENU_TITLE_X + 2, MENU_TITLE_Y + 2, MENU_TITLE_W - 4,
                     MENU_TITLE_H - 4, MENU_TITLE_R - 1, C_TITLE_FILL);
        int tw = r_text_width(f->dialog, m->title);
        int tx = MENU_TITLE_X + (MENU_TITLE_W - tw) / 2;
        if (tx < MENU_TITLE_X + 6) {
            tx = MENU_TITLE_X + 6;
        }
        r_text(s, tx, MENU_TITLE_Y
               + row_text_dy(f->dialog, MENU_TITLE_H),
               f->dialog, m->title, C_WHITE);
    }

    if (m->big) {
        menu_big(s, f, m);
    } else {
        /* 板高随内容长：主菜单两项就矮矮一块，队伍配置撑到 6 行。
         * 固定画到 8 行高会让主菜单下面空一大片，看着像有内容没画出来。
         * 行高按屏取：图标屏 37、无图标屏仍是 28（row_h=0 → 默认）。 */
        int shown = menu_shown(m);
        if (shown < 1) {
            shown = 1;      /* 一项都没有也画个空板：屏幕上只剩背景像渲染坏了 */
        }
        int row_h = m->row_h > 0 ? m->row_h : MENU_ROW_H;
        int hl_h  = row_h - 2;      /* 与 MENU_HL_H 同样的「上下各让 1px」 */
        int box_h = MENU_BOX_PAD_Y * 2 + shown * row_h;
        ui_dialog(s, MENU_BOX_X, MENU_BOX_Y, MENU_BOX_W, box_h, MENU_BOX_R);

        MenuBox b = { MENU_BOX_X, MENU_BOX_Y + MENU_BOX_PAD_Y, MENU_BOX_W,
                      row_h, hl_h, MENU_NOTE_R };
        menu_list(s, f, &b, m);
        menu_scrollbar(s, m, MENU_BOX_Y + MENU_BAR_PAD + 3,
                       box_h - (MENU_BAR_PAD + 3) * 2);
    }

    /* 状态行：与战斗界面**同一处版式**，这样开/关弹层时它不动 */
    ui_dialog(s, STATUS_X, STATUS_Y, STATUS_W, STATUS_H, 5);
    if (f->dialog) {
        if (m->status_left) {
            r_text(s, STATUS_X + 10, STATUS_Y + 4, f->dialog,
                   m->status_left, C_DLG_GOLD);
        }
        if (m->status_right) {
            r_text(s, STATUS_TEXT_X, STATUS_Y + 4, f->dialog,
                   m->status_right, C_DLG_TEXT);
        }
    }
}

void ui_overlay_screen(Surface *s, const UiFonts *f, const MenuScreen *m)
{
    if (!s || !f || !m) {
        return;
    }

    /*
     * 金色模态外框。弹层与招式菜单**占同一个矩形**，两者都是白底蓝框 ——
     * 光看框分不出「我在弹层里」还是「我还在选招式」。这一圈金边就是那个提示，
     * 而且它靠 ui_overlay_rect() 报告的重绘区一定包含得住。
     */
    r_round_rect_outline(s, OVL_X - OVL_MODAL_PAD, OVL_Y - OVL_MODAL_PAD,
                         OVL_W + OVL_MODAL_PAD * 2, OVL_H + OVL_MODAL_PAD * 2,
                         OVL_R + OVL_MODAL_PAD, C_SEL_EDGE,
                         OVL_MODAL_PAD, R_NO_FILL);

    ui_dialog(s, OVL_X, OVL_Y, OVL_W, OVL_H, OVL_R);

    /* 标题在框内第一行。招式框只有 64px 高，塞不下「横条 + 两行」，
     * 所以标题缩成一行小字（12px 字号）—— 这样两行选项仍然是 16px，
     * 与招式菜单一样大，选起来手感一致。 */
    if (m->title && f->box) {
        r_text(s, OVL_X + OVL_TITLE_DX, OVL_Y + 1, f->box, m->title,
               C_DLG_GOLD);
    }

    MenuBox b = { OVL_X, OVL_Y + OVL_HDR_H, OVL_W, OVL_ROW_H, OVL_HL_H,
                  OVL_NOTE_R };
    menu_list(s, f, &b, m);
}
