/*
 * 战斗界面绘制：按三代（火红/叶绿）原版版式。
 *
 * 这一层比 poke_render 高一档 —— 它知道什么是「血条框」「对话框」，
 * 但不关心输入与状态机（那是 poke_ui_state）。
 *
 * 版式依据
 * -------
 * **战斗界面**的尺寸与配色都来自 pret/pokefirered 反编译仓，不是凭印象：
 *
 *   graphics/battle_interface/healthbox.pal   血条框配色
 *   graphics/battle_interface/healthbar.pal   HP 条配色
 *   graphics/battle_interface/textbox1.pal    对话框配色
 *   src/battle_interface.c 的 OAM 定义        血条框尺寸（64x64 两片拼合 -> 128 宽）
 *
 * **菜单界面**（主菜单 / 队伍配置 / 换人 / 结算）没有可抄的原版：原版是
 * 240x160 横屏、菜单是满屏滚动的图块界面；这台设备是 240x320 竖屏，
 * 版式是我们自己定的。能沿用的只有原版对话框那套「白底 + 双蓝框」的
 * 视觉语言（复用 ui_dialog）。所以菜单这块的依据写不出文件名来 ——
 * 与其编一个出处，不如把「这是本项目定的」写清楚。
 *
 * 与本项目调整之处已在代码注释里标出「本项目调整」。
 * tools/make_readme_images.py 用 Python 画同一套**战斗**界面（README 配图），
 * 两边的数值必须一致 —— 改动时两边都要改。菜单界面只在 C 侧实现。
 */
#ifndef POKE_UI_H
#define POKE_UI_H

#include "poke_font.h"
#include "poke_render.h"
#include "poke_sprites.h"

/* 屏幕：240x320，正好是 GBA 原生 240x160 的两倍 */
#define UI_SCREEN_W 240
#define UI_SCREEN_H 320
#define UI_BATTLE_H 160      /* 上半屏 = 原版战斗画面高度 */

/*
 * 两个「文字能用多宽」的预算。绘制与折行都要用它，**不要在别处写死**：
 * 一边按绝对坐标、一边按相对偏移算出来的宽度，两边会悄悄差几个像素。
 * 两个值都在 poke_ui.c 里有 _Static_assert 钉住（改框宽会立刻编译不过）。
 *
 *   UI_DLG_TEXT_W     对话框内可写宽度（日志折行用这个）
 *   UI_STATUS_HINT_W  状态行右侧键位提示的可用宽度
 */
#define UI_DLG_TEXT_W     212
#define UI_STATUS_HINT_W  126

/* 界面用到的三套字号。传 NULL 表示该处不画文字。 */
typedef struct {
    const BitmapFont *box;     /* 12px：血条框内的名字与数字 */
    const BitmapFont *dialog;  /* 16px：对话框与招式菜单 */
    const BitmapFont *label;   /* 10px：HP 标签（只含 ASCII 段，1.7 KB） */
} UiFonts;

/* 一屏战斗界面需要的全部数据 */
typedef struct {
    const SpriteEntry *foe_sprite;   /* 对手立绘，NULL 则跳过 */
    const char *foe_name;
    const char *foe_lv;
    int         foe_hp_permil;       /* 0..1000 */

    const SpriteEntry *me_sprite;
    const char *me_name;
    const char *me_lv;
    int         me_hp_permil;
    const char *me_hp_text;          /* 如 "68/153"，NULL 则不显示数字 */

    const char *log[2];              /* 对话框里的两行日志 */
    const char *moves[4];            /* 招式名 */
    const char *move_pp[4];          /* 剩余 PP，纯数字 */
    int         selected;            /* 高亮第几项，-1 = 不高亮 */

    const char *status_left;         /* 状态行左侧（如 "PP 15/15"） */
    const char *status_right;        /* 状态行右侧（键位提示） */
} BattleScreen;

/* 画一整屏战斗界面 */
void ui_battle_screen(Surface *s, const UiFonts *f, const BattleScreen *d);

/* 单独暴露，便于测试与复用 */
void ui_healthbox(Surface *s, const UiFonts *f, int x, int y, int w, int h,
                  const char *name, const char *lv, int ratio_permil,
                  const char *hp_text);
void ui_dialog(Surface *s, int x, int y, int w, int h, int radius);

/* ================================================================ 菜单 */

/*
 * 主菜单 / 队伍配置 / 弹层 / 认输确认 / 换人 / 结算 —— 这几屏都是
 * 「一个标题 + 一串等高行 + 一个状态行」，差别只在**画满全屏**还是
 * **盖在战斗界面上**，所以共用一套数据与一套行距，分两个入口：
 *
 *   ui_menu_screen()     整屏：主菜单 / 队伍配置 / 换人列表 / 结算
 *   ui_overlay_screen()  盖在战斗界面之上：弹层（换人|认输）/ 认输确认
 *
 * 为什么不做成「一个函数 + overlay 开关」
 * ------------------------------------
 * 两者的**版式不同源**，不是同一个框换个位置：
 *   整屏菜单  标题横条 + 会随内容长高的列表板（最高 MENU_ROWS_MAX 行）
 *   弹层      复用招式菜单那个矩形（232x64），标题缩成框内一行小字
 * 一个函数里塞两套尺寸，读的人得先判断自己落在哪个分支里。分开写，
 * 每边各自与状态机的行数对上（ui_screen_rows）。
 *
 * 行数由**调用方按界面给**（m->rows），不是这里写死：队伍配置一屏 8 行、
 * 换人 6 行、主菜单 2 行，同一个函数要都能画。这些数字来自
 * poke_ui_state.c 的 ui_screen_rows()，两边不许各写一套 —— 见
 * tools/check_consistency.py 的 [16] 组。
 */

/* 一屏列表最多几行。取各界面里最大的那个（队伍配置 8 行）。 */
#define UI_MENU_ROWS_MAX 8

/*
 * 结算大字的配色档位。
 *
 * 为什么不让绘制层去**看文字**决定颜色（strcmp(name, "胜利")）：那样
 * 「文案」和「配色」就绑死了，将来把「胜利」改成「你赢了」会静默变成灰底。
 * 颜色是引擎结果决定的，就该由知道结果的那一层（会话层）给出来。
 */
typedef enum {
    UI_BIG_PLAIN = 0,   /* 无倾向：灰底 */
    UI_BIG_WIN,         /* 胜利：红底 */
    UI_BIG_LOSE,        /* 失败：蓝底 */
    UI_BIG_DRAW,        /* 平局：灰底 */
} UiBigStyle;

typedef struct {
    /*
     * 整屏菜单顶部的标题横条。NULL 表示不画横条（弹层走另一条路，
     * 它把标题画在框内）。
     */
    const char *title;

    /*
     * 结算界面用：正中的大字（"胜利" / "失败" / "平局"）。
     * 非 NULL 时**忽略下面的列表** —— 结算没有可选项，画个列表出来
     * 会让人以为能选。NULL 时正常画列表。
     */
    const char *big;
    const char *sub;      /* 大字下方的小字，可 NULL */
    UiBigStyle  big_style;

    const char *items[UI_MENU_ROWS_MAX];   /* 各行的文字 */
    const char *notes[UI_MENU_ROWS_MAX];   /* 各行右侧的小字（PP / HP），可 NULL */

    int  count;           /* 一共有几项（可能远多于 rows） */
    int  top;             /* 滚动窗口的首项序号 */
    int  rows;            /* 这一屏列几行（<= UI_MENU_ROWS_MAX） */
    int  selected;        /* 高亮第几项（**绝对序号**，不是窗口内偏移）；-1 = 不高亮 */

    bool scrollbar;       /* count > rows 时画右侧滚动条 */

    const char *status_left;   /* 状态行左侧 */
    const char *status_right;  /* 状态行右侧（键位提示） */
} MenuScreen;

/* 整屏列表界面。画之前不要求屏上有什么东西，它自己铺满。 */
void ui_menu_screen(Surface *s, const UiFonts *f, const MenuScreen *m);

/*
 * 画在战斗界面之上。**调用方要先把 ui_battle_screen() 画好** ——
 * 弹层是盖上去的，不是替换：对话框里的战况日志在弹层打开时仍然要看得见，
 * 玩家才知道自己在为什么做选择。
 */
void ui_overlay_screen(Surface *s, const UiFonts *f, const MenuScreen *m);

/*
 * 弹层占的矩形（屏幕坐标）。既用来画，也用来告诉设备侧「哪一块要重绘」。
 * 它**正好等于招式菜单的矩形**：打开弹层就该把招式菜单盖住，
 * 不然两层菜单叠在一起，看的人不知道光标在哪一层。
 */
void ui_overlay_rect(int *x, int *y, int *w, int *h);

#endif /* POKE_UI_H */
