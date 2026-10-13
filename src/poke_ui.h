/*
 * 战斗界面绘制：按三代（火红/叶绿）原版版式。
 *
 * 这一层比 poke_render 高一档 —— 它知道什么是「血条框」「对话框」，
 * 但不关心输入与状态机（那是 poke_ui_state，后续做）。
 *
 * 版式依据
 * -------
 * 所有尺寸与配色都来自 pret/pokefirered 反编译仓，不是凭印象：
 *
 *   graphics/battle_interface/healthbox.pal   血条框配色
 *   graphics/battle_interface/healthbar.pal   HP 条配色
 *   graphics/battle_interface/textbox1.pal    对话框配色
 *   src/battle_interface.c 的 OAM 定义        血条框尺寸（64x64 两片拼合 -> 128 宽）
 *
 * 与本项目调整之处已在代码注释里标出「本项目调整」。
 * tools/make_readme_images.py 用 Python 画同一套界面（README 配图），
 * 两边的数值必须一致 —— 改动时两边都要改。
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

#endif /* POKE_UI_H */
