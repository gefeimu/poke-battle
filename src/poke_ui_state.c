/*
 * 界面状态机实现。设计与取键规则见 poke_ui_state.h。
 */
#include "poke_ui_state.h"

#include <stddef.h>

/* ================================================================ 版面参数 */

int ui_screen_rows(UiScreen sc)
{
    switch (sc) {
    case UI_SCREEN_MAIN:   return 2;
    case UI_SCREEN_TEAM:   return 8;   /* 图鉴列表一屏 8 行 */
    case UI_SCREEN_BATTLE: return 2;   /* 招式菜单是 2x2，占两行 */
    case UI_SCREEN_ACTION: return 2;
    case UI_SCREEN_SWITCH: return 6;   /* 队伍最多 6 只 */
    case UI_SCREEN_RESULT: return 1;
    default:               return 1;
    }
}

/*
 * 每个界面有多少项。
 *
 * 注意**菜单序号 ≠ 数据下标**：招式菜单只列有招式的槽、换人菜单只列还活着的，
 * 所以项数来自 model 而不是固定值。
 */
static int count_of(const UiState *s, UiScreen sc)
{
    switch (sc) {
    case UI_SCREEN_MAIN:   return UI_MAIN_COUNT;
    case UI_SCREEN_TEAM:
        return s->model.species_count > 0 ? s->model.species_count : UI_TEAM_COUNT;
    case UI_SCREEN_BATTLE: return s->model.move_count;
    case UI_SCREEN_ACTION: return UI_ACTION_COUNT;
    case UI_SCREEN_SWITCH: return s->model.switch_count;
    case UI_SCREEN_RESULT: return 1;
    default:               return 1;
    }
}

/* ================================================================ 文字 */

const char *ui_screen_title(UiScreen sc)
{
    switch (sc) {
    case UI_SCREEN_MAIN:   return "主菜单";
    case UI_SCREEN_TEAM:   return "队伍配置";
    case UI_SCREEN_BATTLE: return "招式";
    case UI_SCREEN_ACTION: return "选择";
    case UI_SCREEN_SWITCH: return "换人";
    case UI_SCREEN_RESULT: return "结果";
    default:               return "";
    }
}

const char *ui_item_label(UiScreen sc, int index)
{
    if (sc == UI_SCREEN_MAIN) {
        switch (index) {
        case UI_MAIN_BATTLE: return "开始对战";
        case UI_MAIN_TEAM:   return "队伍配置";
        default:             return NULL;
        }
    }
    if (sc == UI_SCREEN_ACTION) {
        switch (index) {
        case UI_ACTION_SWITCH:  return "换人";
        case UI_ACTION_FORFEIT: return "认输";
        default:                return NULL;
        }
    }
    /* 队伍 / 换人 / 招式 这几行的文字来自数据（种族名、招式名），不在这里写死 */
    return NULL;
}

const char *ui_screen_hint(UiScreen sc)
{
    switch (sc) {
    case UI_SCREEN_MAIN:   return "选择   确定";
    case UI_SCREEN_TEAM:   return "滚动   确定   长按返回";
    case UI_SCREEN_BATTLE: return "选择招式   长按换人";
    case UI_SCREEN_ACTION: return "选择   确定   长按返回";
    case UI_SCREEN_SWITCH: return "选择   确定   长按返回";
    case UI_SCREEN_RESULT: return "确定继续";
    default:               return "";
    }
}

/* ================================================================ 内部 */

static void fix_frame(const UiState *s, UiFrame *f)
{
    int n = count_of(s, f->screen);
    int rows = ui_screen_rows(f->screen);
    if (rows < 1) {
        rows = 1;
    }
    if (n <= 0) {
        f->cursor = 0;
        f->top = 0;
        return;
    }
    if (f->cursor < 0) {
        f->cursor = 0;
    }
    if (f->cursor >= n) {
        f->cursor = (int16_t)(n - 1);
    }
    /* 先把滚动窗口夹进合法范围，再按「光标必须可见」调整 */
    if (f->top < 0) {
        f->top = 0;
    }
    if (n > rows && f->top > n - rows) {
        f->top = (int16_t)(n - rows);
    }
    if (f->cursor < f->top) {
        f->top = f->cursor;
    }
    if (f->cursor >= f->top + rows) {
        f->top = (int16_t)(f->cursor - rows + 1);
    }
}

static void push_screen(UiState *s, UiScreen sc, bool forced)
{
    if (s->depth >= UI_STACK_MAX) {
        return;   /* 栈满就拒绝，绝不覆盖当前界面 */
    }
    UiFrame *f = &s->stack[s->depth++];
    f->screen = sc;
    f->cursor = 0;
    f->top = 0;
    f->result = 0;
    f->forced = forced;
    s->transitions++;
}

static void pop_screen(UiState *s)
{
    if (s->depth > 1) {
        s->depth--;
        s->transitions++;
    }
}

static void reset_to(UiState *s, UiScreen sc)
{
    s->depth = 0;
    push_screen(s, sc, false);
}

static bool emit(UiResult *out, UiAction a, int arg)
{
    if (out) {
        out->act = a;
        out->arg = arg;
    }
    return true;
}

static void move_cursor(UiState *s, int delta)
{
    UiFrame *f = &s->stack[s->depth - 1];
    int n = count_of(s, f->screen);
    if (n <= 1) {
        return;
    }
    /* 环绕移动：4 项菜单里从第一项往上会回到最后一项。
     * 386 项的图鉴列表同理，这样不会出现「顶到头就卡住」的死角。 */
    int c = (int)f->cursor + delta;
    if (c < 0) {
        c += n;
    }
    if (c >= n) {
        c -= n;
    }
    f->cursor = (int16_t)c;
    fix_frame(s, f);
}

/* 当前光标所在的项，换算成数据下标 */
static int cursor_slot(const UiState *s)
{
    const UiFrame *f = &s->stack[s->depth - 1];
    switch (f->screen) {
    case UI_SCREEN_BATTLE:
        return s->model.move_slot[f->cursor];
    case UI_SCREEN_SWITCH:
        return s->model.switch_slot[f->cursor];
    default:
        return f->cursor;
    }
}

/* ================================================================ 对外 */

void ui_state_init(UiState *s)
{
    if (!s) {
        return;
    }
    *s = (UiState){ 0 };
    s->held = -1;
    s->long_key = -1;
    /* 默认模型：4 个招式槽按顺序排、无换人目标、图鉴满列表。
     * 真机上调用方会在每回合开始前用 ui_state_set_model() 覆盖。 */
    s->model.move_count = 4;
    for (int i = 0; i < 4; i++) {
        s->model.move_slot[i] = (uint8_t)i;
    }
    s->model.species_count = UI_TEAM_COUNT;
    reset_to(s, UI_SCREEN_MAIN);
    s->transitions = 0;
}

void ui_state_set_model(UiState *s, const UiModel *m)
{
    if (!s || !m) {
        return;
    }
    s->model = *m;
    /* 数据一变，各层的项数可能变少（招式 PP 耗尽、同队濒死），
     * 光标要跟着夹回合法范围，否则会指到不存在的项上。 */
    for (uint8_t i = 0; i < s->depth; i++) {
        fix_frame(s, &s->stack[i]);
    }
}

void ui_state_begin_battle(UiState *s)
{
    if (!s) {
        return;
    }
    reset_to(s, UI_SCREEN_BATTLE);
}

void ui_state_show_result(UiState *s, int battle_result)
{
    if (!s) {
        return;
    }
    reset_to(s, UI_SCREEN_RESULT);
    s->stack[0].result = (int16_t)battle_result;
}

void ui_state_force_switch(UiState *s, const uint8_t *slots, int n)
{
    if (!s) {
        return;
    }
    if (n < 0) {
        n = 0;
    }
    if (n > UI_TEAM_SLOTS) {
        n = UI_TEAM_SLOTS;
    }
    for (int i = 0; i < n; i++) {
        s->model.switch_slot[i] = slots ? slots[i] : (uint8_t)i;
    }
    for (int i = n; i < UI_TEAM_SLOTS; i++) {
        s->model.switch_slot[i] = 0;
    }
    s->model.switch_count = (uint8_t)n;
    /* 强制换人：长按无效（forced），没有退路 */
    reset_to(s, UI_SCREEN_SWITCH);
    s->stack[0].forced = true;
}

bool ui_state_feed(UiState *s, const KeyMsg *msg, uint32_t now_ms, UiResult *out)
{
    if (out) {
        out->act = UI_ACT_NONE;
        out->arg = 0;
    }
    if (!s || !msg || s->depth == 0) {
        return false;
    }

    UiFrame *f = &s->stack[s->depth - 1];

    /* 记录按住状态，供 ui_state_tick 连发用。RELEASE 是这里唯一的用处 ——
     * 它不是指令，只是「键不再被按住」的通知。 */
    if (msg->ev == KEY_EV_PRESS) {
        s->held = (int8_t)msg->key;
        s->held_at = now_ms;
        s->repeat_at = now_ms;
        s->long_key = -1;
    } else if (msg->ev == KEY_EV_RELEASE) {
        s->held = -1;
        s->long_key = -1;
    } else if (msg->ev == KEY_EV_LONG) {
        s->long_key = (int8_t)msg->key;
    }

    /*
     * 由长按触发的动作，必须吞掉紧随其后的那次抬起 —— 否则「长按返回」
     * 会顺手把新界面上光标所指那一项也确定一次。官方文档把这列为一条
     * 必须保留的规则。
     *
     * 我们自己的 poke_input 在报过 LONG 之后不会再补 CLICK，但官方按键组件
     * 的某些配置会两条都报。这一层不该指望上游，所以自己挡一道。
     */
    if (msg->ev == KEY_EV_CLICK && (int)s->long_key == (int)msg->key) {
        s->long_key = -1;
        return false;
    }

    switch (msg->ev) {
    case KEY_EV_PRESS:
        /* 走动放在 PRESS 上：按下立刻动才跟手。连发不在这里产生，
         * 由 ui_state_tick 按自己的节奏走（驱动只上报边沿）。 */
        if (msg->key == KEY_UP) {
            move_cursor(s, -1);
        } else if (msg->key == KEY_DOWN) {
            move_cursor(s, +1);
        }
        /* OK 的按下不做事：否则「长按返回」会先顺手确定一次 */
        return false;

    case KEY_EV_LONG:
        if (msg->key != KEY_OK) {
            return false;   /* ▲▼ 长按只是持续移动，交给自己 tick */
        }
        if (f->forced) {
            return false;   /* 强制换人没有退路 */
        }
        if (f->screen == UI_SCREEN_BATTLE) {
            /* 战斗主界面是流程的最底层，没有上一层可返回，
             * 于是长按在这里改为「打开弹层」。 */
            push_screen(s, UI_SCREEN_ACTION, false);
            return false;
        }
        if (f->screen == UI_SCREEN_RESULT) {
            reset_to(s, UI_SCREEN_MAIN);
            return false;
        }
        pop_screen(s);
        return false;

    case KEY_EV_CLICK:
        break;   /* 下面按界面分派 */

    case KEY_EV_RELEASE:
    case KEY_EV_NONE:
    default:
        return false;
    }

    if (msg->key != KEY_OK) {
        /* ▲▼ 的短按已经在 PRESS 时走过了，这里什么都不做 ——
         * 若也移动一次，一次轻点就会跳两格。 */
        return false;
    }

    switch (f->screen) {
    case UI_SCREEN_MAIN:
        if (f->cursor == UI_MAIN_BATTLE) {
            push_screen(s, UI_SCREEN_BATTLE, false);
            return emit(out, UI_ACT_START_BATTLE, 0);
        }
        push_screen(s, UI_SCREEN_TEAM, false);
        return false;

    case UI_SCREEN_TEAM:
        /* 逐只编辑（性格/努力值/招式…）是后面的事，先把「选了哪只」报上去 */
        return emit(out, UI_ACT_PICK_SPECIES, f->cursor);

    case UI_SCREEN_BATTLE:
        if (f->cursor >= count_of(s, UI_SCREEN_BATTLE)) {
            return false;   /* 没有可用招式（本回合无 PP）时不给动作 */
        }
        return emit(out, UI_ACT_CHOOSE_MOVE, cursor_slot(s));

    case UI_SCREEN_ACTION:
        if (f->cursor == UI_ACTION_SWITCH) {
            push_screen(s, UI_SCREEN_SWITCH, false);
            return false;
        }
        /* 认输不可逆：本层只报「用户想认输」，真的结束对局由调用方决定 */
        return emit(out, UI_ACT_FORFEIT, 0);

    case UI_SCREEN_SWITCH:
        if (f->cursor >= count_of(s, UI_SCREEN_SWITCH)) {
            return false;
        }
        {
            int slot = cursor_slot(s);
            /* 换人消耗回合：选完就回战斗界面。强制的与自愿的都一样。 */
            ui_state_begin_battle(s);
            return emit(out, UI_ACT_SWITCH_MON, slot);
        }

    case UI_SCREEN_RESULT:
        reset_to(s, UI_SCREEN_MAIN);
        return false;

    default:
        return false;
    }
}

bool ui_state_tick(UiState *s, uint32_t now_ms, UiResult *out)
{
    if (out) {
        out->act = UI_ACT_NONE;
        out->arg = 0;
    }
    if (!s || (s->held != KEY_UP && s->held != KEY_DOWN)) {
        return false;
    }
    if (now_ms - s->held_at < UI_REPEAT_DELAY_MS) {
        return false;
    }
    if (now_ms - s->repeat_at < UI_REPEAT_RATE_MS) {
        return false;
    }
    s->repeat_at = now_ms;
    move_cursor(s, s->held == KEY_DOWN ? +1 : -1);
    return false;
}

/* ================================================================ 查询 */

UiScreen ui_state_screen(const UiState *s)
{
    return s && s->depth > 0 ? s->stack[s->depth - 1].screen : UI_SCREEN_MAIN;
}

int ui_state_cursor(const UiState *s)
{
    return s && s->depth > 0 ? s->stack[s->depth - 1].cursor : 0;
}

int ui_state_top(const UiState *s)
{
    return s && s->depth > 0 ? s->stack[s->depth - 1].top : 0;
}

int ui_state_count(const UiState *s)
{
    return s && s->depth > 0 ? count_of(s, s->stack[s->depth - 1].screen) : 0;
}

bool ui_state_forced(const UiState *s)
{
    return s && s->depth > 0 && s->stack[s->depth - 1].forced;
}

int ui_state_depth(const UiState *s)
{
    return s ? (int)s->depth : 0;
}

int ui_state_result(const UiState *s)
{
    return s && s->depth > 0 ? s->stack[s->depth - 1].result : 0;
}
