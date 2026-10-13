/*
 * 界面状态机：三键 → 界面切换 + 光标移动 + 想做的动作。
 *
 * 定位
 * ----
 * 这一层**不画像素，也不碰战斗引擎**：
 *   - 上游吃 poke_input 的按键事件（KeyMsg）；
 *   - 下游吐出「用户想做什么」（UiResult 里的 UiAction），由调用方翻译成
 *     battle_step() / battle_switch() / battle_mega() 的调用（那是路线图第 3 项）。
 * 这样整个导航逻辑可以在主机侧用一串按键走完，既不用屏幕也不用引擎 ——
 * 与渲染层「同一份数值、两套实现」的思路一致。
 *
 * 三键怎么用（本项目约定）
 * -----------------------
 *   ▲ / ▼      移动光标。按住 400 ms 后连发，每 120 ms 一格
 *   OK 短按    确定
 *   OK 长按    返回上一层；**在战斗主界面没有上一层，改为打开弹层**
 *
 * 「长按=返回，无处可返回时打开弹层」是一条规则不是两条特例：战斗一旦开始
 * 就不能退回主菜单，战斗主界面因此是流程的最底层。
 *
 * 为什么 ▲▼ 用 PRESS 而 OK 用 CLICK
 * ---------------------------------
 * 走动作用在 PRESS 上（按下立刻动才跟手），连发由本层 tick 出来；
 * OK 必须用 CLICK —— PRESS 在长按时也会先发一次，用 PRESS 就会
 * 「长按返回」的同时还顺手确定一次。这也是官方文档的结论：只有 CLICK 与
 * LONG 算用户意图，RELEASE 只表示「键不再被按住」。
 *
 * 连发为什么在本层而不是驱动层
 * ---------------------------
 * 官方文档明确写了：驱动只上报边沿，「按住重复」是应用自己的 tick。
 * 所以 poke_input 不产生连发事件，本层用 ui_state_tick() 按自己的节奏走。
 */
#ifndef POKE_UI_STATE_H
#define POKE_UI_STATE_H

#include <stdbool.h>
#include <stdint.h>

#include "poke_input.h"

/* ---------------------------------------------------------------- 界面 */

typedef enum {
    UI_SCREEN_MAIN = 0,  /* 主菜单：开始对战 / 队伍配置 */
    UI_SCREEN_TEAM,      /* 队伍配置：图鉴列表（可滚动） */
    UI_SCREEN_BATTLE,    /* 战斗主界面：招式菜单（2x2，与 hero 图一致） */
    UI_SCREEN_ACTION,    /* 战斗中弹层：换人 / 逃跑 */
    UI_SCREEN_SWITCH,    /* 换人选择 */
    UI_SCREEN_RESULT,    /* 结算 */
    UI_SCREEN_COUNT,
} UiScreen;

/* 弹层的项。顺序即列表顺序 —— **不要在中间插项**，会改变已存界面的光标含义。 */
enum {
    UI_ACTION_SWITCH = 0,
    UI_ACTION_RUN,
    UI_ACTION_COUNT,
};

/* 主菜单的项 */
enum {
    UI_MAIN_BATTLE = 0,
    UI_MAIN_TEAM,
    UI_MAIN_COUNT,
};

/* ---------------------------------------------------------------- 动作 */

/*
 * 状态机不执行这些动作，只声明「用户想做」。调用方（第 3 项「接引擎」）
 * 负责把它们翻成引擎调用。
 */
typedef enum {
    UI_ACT_NONE = 0,
    UI_ACT_START_BATTLE,   /* 开始对战（调用方装队 + battle_start） */
    UI_ACT_PICK_SPECIES,   /* arg = 图鉴序号；队伍编辑尚未实现，先报出来 */
    UI_ACT_CHOOSE_MOVE,    /* arg = **招式槽** 0..3（不是菜单序号） */
    UI_ACT_SWITCH_MON,     /* arg = **队伍槽** 0..5（不是菜单序号） */
    UI_ACT_RUN,            /* 逃跑 */
} UiAction;

typedef struct {
    UiAction act;
    int      arg;
} UiResult;

/* ---------------------------------------------------------------- 模型 */

#define UI_TEAM_COUNT  386   /* 图鉴只数（正面立绘 386 张），即队伍配置列表长度 */
#define UI_TEAM_SLOTS  6     /* 队伍 6 只。刻意不 include poke_battle.h：这一层不该依赖引擎 */

/*
 * 界面要展示的数据由调用方填。**菜单序号 ≠ 数据下标**，所以要靠下面的
 * 映射表把「第几项」翻成「第几个招式槽 / 第几个队伍槽」：
 * 一只宝可梦可能只带 2 个招式、队伍里可能只有 3 只还活着，空槽要跳过。
 */
typedef struct {
    uint8_t move_slot[4];                    /* 菜单第 i 项 → 招式槽 */
    uint8_t move_count;                      /* 有招式的项数，1..4 */
    uint8_t switch_slot[UI_TEAM_SLOTS];      /* 换人菜单第 i 项 → 队伍槽（只放还活着的） */
    uint8_t switch_count;                    /* 0..6 */
    int16_t species_count;                   /* 队伍配置列表长度，<=0 时按 UI_TEAM_COUNT */
} UiModel;

/* ---------------------------------------------------------------- 状态 */

/*
 * 界面栈深度。当前最深的一条路是 4 层：
 *   主菜单 → 战斗主界面 → 弹层 → 换人列表
 * 留到 6 是为了后面加界面（招式详情、背包、Mega 确认）时不用回来改数字。
 * 满了以后再 push 会**静默忽略**（不覆盖当前界面）—— 那是设计上的取舍：
 * 覆盖会让用户莫名其妙跳界面，忽略最多是「按了没反应」。
 */
#define UI_STACK_MAX 6

typedef struct {
    UiScreen screen;
    int16_t  cursor;   /* 当前选中第几项 */
    int16_t  top;      /* 列表滚动窗口的首项 */
    int16_t  result;   /* 只对结算界面有意义：引擎的 BattleResult */
    bool     forced;   /* 强制换人：长按无效，必须选一只 */
} UiFrame;

/* 按住连发节奏。首格由 PRESS 立刻走出，这两项只管后续连发。 */
#define UI_REPEAT_DELAY_MS 400
#define UI_REPEAT_RATE_MS  120

typedef struct {
    UiFrame  stack[UI_STACK_MAX];
    uint8_t  depth;      /* stack[depth-1] 是当前界面；depth >= 1 */
    UiModel  model;

    int8_t   held;       /* 当前按住的键，-1 = 无 */
    uint32_t held_at;
    uint32_t repeat_at;
    int8_t   long_key;   /* 最近报过长按的键，-1 = 无。用来吞掉紧随其后的 CLICK */

    uint32_t transitions;   /* 界面切换次数（自检用） */
} UiState;

/* ---------------------------------------------------------------- 接口 */

void ui_state_init(UiState *s);

/* 更新要展示的数据；会顺带把各层光标夹回合法范围（招式变少、换人列表变短时） */
void ui_state_set_model(UiState *s, const UiModel *m);

/* 进入战斗主界面（栈重置）。换人结束后要回到战斗界面时也调它。 */
void ui_state_begin_battle(UiState *s);

/* 引擎报出结果 → 结算界面 */
void ui_state_show_result(UiState *s, int battle_result);

/*
 * 强制换人：出战的那只濒死，必须先换人才能继续。
 * 进入一个 forced 的换人界面，长按无效（没有退路）。
 * slots/n 描述可选的队伍槽；填进 model。
 */
void ui_state_force_switch(UiState *s, const uint8_t *slots, int n);

/*
 * 喂一条按键事件。产生动作时写进 out（out 可为 NULL）并返回 true。
 * now_ms 必须单调递增（同 key_poll）。
 */
bool ui_state_feed(UiState *s, const KeyMsg *msg, uint32_t now_ms, UiResult *out);

/*
 * 按住连发。需要周期性调用（设备侧每 16 ms 一次）。
 * 只有 ▲▼ 会连发；OK 的按住由 LONG 事件表达。
 */
bool ui_state_tick(UiState *s, uint32_t now_ms, UiResult *out);

/* ---- 查询（绘制层与测试共用）---- */

UiScreen ui_state_screen(const UiState *s);
int      ui_state_cursor(const UiState *s);
int      ui_state_top(const UiState *s);
int      ui_state_count(const UiState *s);
bool     ui_state_forced(const UiState *s);
int      ui_state_depth(const UiState *s);

/* 结算界面上引擎给的结果码（BattleResult）；其余界面返回 0 */
int      ui_state_result(const UiState *s);

/* 该界面一屏能列几行（滚动窗口高度） */
int ui_screen_rows(UiScreen sc);

/*
 * 文字。集中在这里定义，绘制层与自检取同一份 —— 避免两处各写一套、
 * 改了一处忘了另一处（字库覆盖检查也要用它们）。
 * 列表类界面（队伍/换人）的行文字来自数据，这里返回 NULL。
 */
const char *ui_screen_title(UiScreen sc);
const char *ui_item_label(UiScreen sc, int index);

/* 状态行右侧的键位提示 */
const char *ui_screen_hint(UiScreen sc);

#endif /* POKE_UI_STATE_H */
