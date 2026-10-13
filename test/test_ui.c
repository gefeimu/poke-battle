/*
 * test_ui.c - 界面状态机自检
 *
 * 这一层的 bug 有个共同形状：**画面没坏、程序没崩，只是按键到了错误的界面**。
 * 「长按返回」顺手把新界面上光标那项也确定了一次、招式菜单的序号没换算成招式槽、
 * 数据变少之后光标还指着不存在的项 —— 全属于这一类，只有喂按键序列才测得出来。
 *
 * 所以这里不复现渲染，也不装战斗引擎，只做一件事：
 * 喂一串按键，断言「界面 / 光标 / 滚动窗口 / 吐出的动作」。
 *
 * 断言里另一半是**取键规则**：▲▼ 走 PRESS、OK 走 CLICK、长按吞掉紧随的 CLICK。
 * 这几条错了就是「手感不对」，而手感在真机上更没法测。
 */
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "poke_ui_state.h"

static int g_pass = 0, g_fail = 0;

static void check(int cond, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    char buf[240];
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

const char *screen_name(UiScreen sc);
const char *action_name(UiAction a);

/* ================================================================ 试验台 */

typedef struct {
    UiState  st;
    uint32_t t;
    UiResult last;      /* 最近一次产生的动作 */
    int      nactions;  /* 动作总数 */
} Rig;

static void rig_init(Rig *r)
{
    ui_state_init(&r->st);
    r->t = 0;
    r->last = (UiResult){ UI_ACT_NONE, 0 };
    r->nactions = 0;
}

static bool ev_(Rig *r, KeyId k, KeyEvent e, UiResult *out)
{
    r->t += 16;
    KeyMsg m = { .key = k, .ev = e, .mv = 0, .t_ms = r->t };
    UiResult res = { UI_ACT_NONE, 0 };
    bool got = ui_state_feed(&r->st, &m, r->t, &res);
    if (got) {
        r->last = res;
        r->nactions++;
    }
    if (out) {
        *out = res;
    }
    return got;
}

/* 一次完整轻点：按下 → 短按 → 抬起 */
static bool tap(Rig *r, KeyId k, UiResult *out)
{
    ev_(r, k, KEY_EV_PRESS, NULL);
    bool got = ev_(r, k, KEY_EV_CLICK, out);
    ev_(r, k, KEY_EV_RELEASE, NULL);
    return got;
}

/* 一次长按：按下 → 长按 → 抬起 */
static void hold(Rig *r, KeyId k)
{
    ev_(r, k, KEY_EV_PRESS, NULL);
    ev_(r, k, KEY_EV_LONG, NULL);
    ev_(r, k, KEY_EV_RELEASE, NULL);
}

/* 把时基推到某个绝对时刻再 tick。连发是按绝对时刻判定的，
 * 用「推进到 t」而不是「推进 dt」，断言才不会被前面几次 ev_ 的累加搞乱。 */
static void tick_to(Rig *r, uint32_t t)
{
    if (t > r->t) {
        r->t = t;
    }
    UiResult res = { UI_ACT_NONE, 0 };
    ui_state_tick(&r->st, r->t, &res);
}

static void tick(Rig *r, uint32_t dt)
{
    tick_to(r, r->t + dt);
}

/* 造一份模型：招式只留 3 个槽（跳过槽 2），换人只列 4 只活着的 */
static UiModel model_sample(void)
{
    UiModel m;
    memset(&m, 0, sizeof(m));
    m.move_count = 3;
    m.move_slot[0] = 0;
    m.move_slot[1] = 1;
    m.move_slot[2] = 3;
    m.switch_count = 4;
    m.switch_slot[0] = 0;
    m.switch_slot[1] = 2;
    m.switch_slot[2] = 4;
    m.switch_slot[3] = 5;
    m.species_count = UI_TEAM_COUNT;
    return m;
}

/* ================================================================ 用例 */

static void test_init(void)
{
    printf("\n[初始状态]\n");
    UiState s;
    ui_state_init(&s);
    check(ui_state_screen(&s) == UI_SCREEN_MAIN, "开局停在主菜单（%s）",
          screen_name(ui_state_screen(&s)));
    check(ui_state_cursor(&s) == 0, "光标在第 0 项");
    check(ui_state_count(&s) == UI_MAIN_COUNT, "主菜单 %d 项", ui_state_count(&s));
    check(ui_state_depth(&s) == 1, "栈深 1（当前界面就是栈底）");
    check(ui_state_top(&s) == 0, "滚动窗口从第 0 项开始");
    check(!ui_state_forced(&s), "非强制换人");
    check(ui_state_result(&s) == 0, "非结算界面没有结果码");
    check(s.model.move_count == 4, "默认模型给 4 个招式槽（%d）", s.model.move_count);
}

static void test_main_nav(void)
{
    printf("\n[主菜单] ▲▼ 用 PRESS 走动，抬起不再动\n");
    Rig r;
    rig_init(&r);

    ev_(&r, KEY_UP, KEY_EV_PRESS, NULL);
    check(ui_state_cursor(&r.st) == 1, "第 0 项按上 → 环绕到最后一项（%d）",
          ui_state_cursor(&r.st));
    ev_(&r, KEY_DOWN, KEY_EV_PRESS, NULL);
    check(ui_state_cursor(&r.st) == 0, "按下 → 回到第 0 项");

    /* 走动只走 PRESS。抬起时再走一次就是「一次轻点跳两格」，是这类界面的经典 bug。 */
    ev_(&r, KEY_DOWN, KEY_EV_PRESS, NULL);
    check(ui_state_cursor(&r.st) == 1, "按下 → 第 1 项");
    ev_(&r, KEY_DOWN, KEY_EV_CLICK, NULL);
    check(ui_state_cursor(&r.st) == 1, "CLICK 不再移动（仍是第 %d 项）",
          ui_state_cursor(&r.st));
    ev_(&r, KEY_DOWN, KEY_EV_RELEASE, NULL);
    check(ui_state_cursor(&r.st) == 1, "RELEASE 也不再移动（仍是第 %d 项）",
          ui_state_cursor(&r.st));

    /* OK 的 PRESS 不能立刻确定，否则「长按返回」会先顺手确定一次 */
    Rig r2;
    rig_init(&r2);
    ev_(&r2, KEY_OK, KEY_EV_PRESS, NULL);
    check(ui_state_screen(&r2.st) == UI_SCREEN_MAIN && r2.nactions == 0,
          "OK 的 PRESS 不产生动作、也不换界面");

    /* 主菜单没有上一层，长按什么都不做（也不该打开弹层） */
    hold(&r2, KEY_OK);
    check(ui_state_screen(&r2.st) == UI_SCREEN_MAIN && r2.nactions == 0,
          "主菜单上长按无效");
    check(ui_state_depth(&r2.st) == 1, "栈深没变（%d）", ui_state_depth(&r2.st));
}

static void test_team_scroll(void)
{
    printf("\n[队伍配置] 386 项列表的滚动与按住连发\n");
    Rig r;
    rig_init(&r);
    ev_(&r, KEY_DOWN, KEY_EV_PRESS, NULL);      /* 光标移到「队伍配置」 */
    tap(&r, KEY_OK, NULL);
    check(ui_state_screen(&r.st) == UI_SCREEN_TEAM, "进入队伍配置（%s）",
          screen_name(ui_state_screen(&r.st)));
    check(ui_state_count(&r.st) == UI_TEAM_COUNT, "列表 %d 项", ui_state_count(&r.st));
    check(ui_state_top(&r.st) == 0, "滚动窗口从 0 开始");

    /* 按住连发：首格在 PRESS 时已经走过，之后靠 tick。
     * 满 400 ms 起发，之后每 120 ms 一格。 */
    ev_(&r, KEY_DOWN, KEY_EV_PRESS, NULL);
    uint32_t t0 = r.t;               /* 按下时刻 */
    check(ui_state_cursor(&r.st) == 1, "按下立刻走一格（第 %d 项）",
          ui_state_cursor(&r.st));
    tick_to(&r, t0 + UI_REPEAT_DELAY_MS - 1);
    check(ui_state_cursor(&r.st) == 1, "不到 400 ms 不连发（仍是第 %d 项）",
          ui_state_cursor(&r.st));
    tick_to(&r, t0 + UI_REPEAT_DELAY_MS);
    check(ui_state_cursor(&r.st) == 2, "满 400 ms 连发一格（第 %d 项）",
          ui_state_cursor(&r.st));
    tick_to(&r, t0 + UI_REPEAT_DELAY_MS + UI_REPEAT_RATE_MS);
    check(ui_state_cursor(&r.st) == 3, "再过 120 ms 又一格（第 %d 项）",
          ui_state_cursor(&r.st));
    tick_to(&r, t0 + UI_REPEAT_DELAY_MS + UI_REPEAT_RATE_MS * 2);
    check(ui_state_cursor(&r.st) == 4, "连发是持续的（第 %d 项）",
          ui_state_cursor(&r.st));

    /* 抬起必须停住：残留的「按住」会在画面已经走开之后继续推进 */
    ev_(&r, KEY_DOWN, KEY_EV_RELEASE, NULL);
    tick(&r, 1000);
    check(ui_state_cursor(&r.st) == 4, "抬起后不再连发（仍是第 %d 项）",
          ui_state_cursor(&r.st));

    /* OK 不连发 */
    ev_(&r, KEY_OK, KEY_EV_PRESS, NULL);
    tick(&r, 1000);
    check(ui_state_cursor(&r.st) == 4, "OK 按住不连发（仍是第 %d 项）",
          ui_state_cursor(&r.st));
    ev_(&r, KEY_OK, KEY_EV_RELEASE, NULL);

    /* 滚动窗口：一屏 8 行，光标贴到窗口下沿时窗口跟着滚 */
    Rig r3;
    rig_init(&r3);
    ev_(&r3, KEY_DOWN, KEY_EV_PRESS, NULL);
    tap(&r3, KEY_OK, NULL);
    for (int i = 0; i < 10; i++) {
        ev_(&r3, KEY_DOWN, KEY_EV_PRESS, NULL);
    }
    check(ui_state_cursor(&r3.st) == 10, "按 10 次下 → 第 %d 项", ui_state_cursor(&r3.st));
    check(ui_state_top(&r3.st) == 10 - ui_screen_rows(UI_SCREEN_TEAM) + 1,
          "窗口跟着滚，top = %d（应为 %d）", ui_state_top(&r3.st),
          10 - ui_screen_rows(UI_SCREEN_TEAM) + 1);

    /* 从第 0 项往上环绕到最末项，窗口要跟到底 */
    Rig r4;
    rig_init(&r4);
    ev_(&r4, KEY_DOWN, KEY_EV_PRESS, NULL);
    tap(&r4, KEY_OK, NULL);
    ev_(&r4, KEY_UP, KEY_EV_PRESS, NULL);
    check(ui_state_cursor(&r4.st) == UI_TEAM_COUNT - 1,
          "第 0 项按上 → 环绕到最末项（第 %d 项）", ui_state_cursor(&r4.st));
    check(ui_state_top(&r4.st) == UI_TEAM_COUNT - ui_screen_rows(UI_SCREEN_TEAM),
          "窗口跟到底，top = %d（应为 %d）", ui_state_top(&r4.st),
          UI_TEAM_COUNT - ui_screen_rows(UI_SCREEN_TEAM));

    /* 队伍列表里确定 → 报「选了哪只」，界面不动（编辑器还没做） */
    UiResult out;
    bool got = tap(&r4, KEY_OK, &out);
    check(got && out.act == UI_ACT_PICK_SPECIES && out.arg == UI_TEAM_COUNT - 1,
          "确定 → 选了第 %d 只（%s）", out.arg, action_name(out.act));
    check(ui_state_screen(&r4.st) == UI_SCREEN_TEAM, "选完之后仍停在队伍配置");
}

static void test_battle_moves(void)
{
    printf("\n[战斗界面] 招式菜单：菜单序号要换算成招式槽\n");
    Rig r;
    rig_init(&r);
    UiModel m = model_sample();
    ui_state_set_model(&r.st, &m);

    UiResult out;
    bool got = tap(&r, KEY_OK, &out);
    check(got && out.act == UI_ACT_START_BATTLE, "主菜单第 1 项 → 开始对战（%s）",
          action_name(out.act));
    check(ui_state_screen(&r.st) == UI_SCREEN_BATTLE, "进入战斗主界面（%s）",
          screen_name(ui_state_screen(&r.st)));
    check(ui_state_count(&r.st) == 3, "招式菜单 3 项：空的招式槽不占菜单位（%d）",
          ui_state_count(&r.st));

    got = tap(&r, KEY_OK, &out);
    check(got && out.act == UI_ACT_CHOOSE_MOVE && out.arg == 0,
          "选第 1 项 → 招式槽 0（arg=%d）", out.arg);

    ev_(&r, KEY_DOWN, KEY_EV_PRESS, NULL);
    tap(&r, KEY_OK, &out);
    check(out.act == UI_ACT_CHOOSE_MOVE && out.arg == 1,
          "选第 2 项 → 招式槽 1（arg=%d）", out.arg);

    ev_(&r, KEY_DOWN, KEY_EV_PRESS, NULL);
    tap(&r, KEY_OK, &out);
    check(out.act == UI_ACT_CHOOSE_MOVE && out.arg == 3,
          "选第 3 项 → **招式槽 3**（跳过空槽 2，arg=%d）", out.arg);

    /* 招式变少（PP 耗尽）时项数跟着变，光标要夹回去 */
    UiModel m2 = m;
    m2.move_count = 1;
    ui_state_set_model(&r.st, &m2);
    check(ui_state_cursor(&r.st) == 0, "可用招式减到 1 项 → 光标夹回 0（%d）",
          ui_state_cursor(&r.st));
    check(ui_state_count(&r.st) == 1, "项数跟着变成 %d", ui_state_count(&r.st));

    /* 一个招式都没有时不给动作（否则会报出一个无效的招式槽） */
    UiModel m3 = m;
    m3.move_count = 0;
    ui_state_set_model(&r.st, &m3);
    int before = r.nactions;
    got = tap(&r, KEY_OK, &out);
    check(!got && r.nactions == before,
          "无可用招式时确定不报动作（动作数仍是 %d）", r.nactions);
}

static void test_overlay(void)
{
    printf("\n[弹层与换人] 战斗界面上长按 = 打开弹层\n");
    Rig r;
    rig_init(&r);
    UiModel m = model_sample();
    ui_state_set_model(&r.st, &m);
    tap(&r, KEY_OK, NULL);                       /* 开始对战 */
    check(ui_state_screen(&r.st) == UI_SCREEN_BATTLE, "先到战斗主界面");

    hold(&r, KEY_OK);
    check(ui_state_screen(&r.st) == UI_SCREEN_ACTION, "长按 → 打开弹层（%s）",
          screen_name(ui_state_screen(&r.st)));
    /* 主菜单(1) → 战斗界面(2) → 弹层(3) */
    check(ui_state_depth(&r.st) == 3, "弹层压在战斗界面之上（栈深 %d）",
          ui_state_depth(&r.st));
    check(r.nactions == 1, "打开弹层本身不产生动作（动作数 %d）", r.nactions);

    /* 弹层第 1 项「换人」→ 换人列表 */
    UiResult out;
    bool got = tap(&r, KEY_OK, &out);
    check(!got && ui_state_screen(&r.st) == UI_SCREEN_SWITCH,
          "选「换人」→ 进入换人列表（%s）", screen_name(ui_state_screen(&r.st)));
    check(ui_state_depth(&r.st) == 4, "这是最深的一条路：栈深 %d（主菜单→战斗→弹层→换人）",
          ui_state_depth(&r.st));
    check(UI_STACK_MAX > 4, "栈深留了余量（UI_STACK_MAX = %d）", UI_STACK_MAX);
    check(ui_state_count(&r.st) == 4, "只列还活着的 %d 只", ui_state_count(&r.st));

    ev_(&r, KEY_DOWN, KEY_EV_PRESS, NULL);
    got = tap(&r, KEY_OK, &out);
    check(got && out.act == UI_ACT_SWITCH_MON && out.arg == 2,
          "换人列表第 2 项 → 队伍槽 2（arg=%d）", out.arg);
    check(ui_state_screen(&r.st) == UI_SCREEN_BATTLE && ui_state_depth(&r.st) == 1,
          "换人选完回战斗主界面（栈深 %d）", ui_state_depth(&r.st));

    /* 长按在弹层里是「返回」 */
    hold(&r, KEY_OK);
    check(ui_state_screen(&r.st) == UI_SCREEN_ACTION, "从战斗界面长按进弹层");
    hold(&r, KEY_OK);
    check(ui_state_screen(&r.st) == UI_SCREEN_BATTLE, "弹层里长按 → 返回战斗界面");
    check(ui_state_depth(&r.st) == 1, "返回后栈深 %d", ui_state_depth(&r.st));

    /* 弹层第 2 项「认输」—— 只进确认界面，**不产生动作** */
    hold(&r, KEY_OK);
    ev_(&r, KEY_DOWN, KEY_EV_PRESS, NULL);
    int before = r.nactions;
    got = tap(&r, KEY_OK, &out);
    check(!got && r.nactions == before
          && ui_state_screen(&r.st) == UI_SCREEN_CONFIRM,
          "弹层第 2 项 → 进确认界面、不产生动作（%s）",
          screen_name(ui_state_screen(&r.st)));
    check(ui_state_depth(&r.st) == 3,
          "确认界面压在弹层之上（栈深 %d；此处战斗界面就是栈底，刚换过人重置过）",
          ui_state_depth(&r.st));

    /* 队伍配置里长按也是返回 */
    Rig r2;
    rig_init(&r2);
    ev_(&r2, KEY_DOWN, KEY_EV_PRESS, NULL);
    tap(&r2, KEY_OK, NULL);
    check(ui_state_screen(&r2.st) == UI_SCREEN_TEAM, "进队伍配置");
    hold(&r2, KEY_OK);
    check(ui_state_screen(&r2.st) == UI_SCREEN_MAIN && ui_state_depth(&r2.st) == 1,
          "队伍配置里长按 → 回主菜单（%s）", screen_name(ui_state_screen(&r2.st)));
}

static void test_forfeit_confirm(void)
{
    printf("\n[认输确认] 不可逆的操作要两次刻意操作才生效\n");
    Rig r;
    rig_init(&r);
    UiModel m = model_sample();
    ui_state_set_model(&r.st, &m);
    tap(&r, KEY_OK, NULL);                       /* 开始对战（动作 1） */
    hold(&r, KEY_OK);                            /* 打开弹层 */
    check(ui_state_screen(&r.st) == UI_SCREEN_ACTION, "先到弹层");

    /* 弹层默认光标在「换人」上，移到第 2 项「认输」 */
    ev_(&r, KEY_DOWN, KEY_EV_PRESS, NULL);
    int before = r.nactions;
    UiResult out;
    bool got = tap(&r, KEY_OK, &out);
    check(!got && r.nactions == before
          && ui_state_screen(&r.st) == UI_SCREEN_CONFIRM,
          "选「认输」→ 进确认界面（%s）", screen_name(ui_state_screen(&r.st)));
    check(ui_state_cursor(&r.st) == UI_CONFIRM_CANCEL,
          "默认光标停在「取消」（第 %d 项）—— 误按一次不该判负",
          ui_state_cursor(&r.st));
    check(ui_state_count(&r.st) == UI_CONFIRM_COUNT, "确认界面 %d 项",
          ui_state_count(&r.st));
    check(ui_state_depth(&r.st) == 4, "栈深 %d（主菜单→战斗→弹层→确认）",
          ui_state_depth(&r.st));

    /* 光标在「取消」上确定 → 回弹层，什么都不发生 */
    got = tap(&r, KEY_OK, &out);
    check(!got && r.nactions == before && ui_state_screen(&r.st) == UI_SCREEN_ACTION,
          "「取消」→ 回弹层、不产生动作（%s）", screen_name(ui_state_screen(&r.st)));
    check(ui_state_depth(&r.st) == 3, "退回一层（栈深 %d）", ui_state_depth(&r.st));
    check(ui_state_cursor(&r.st) == UI_ACTION_FORFEIT,
          "弹层的光标还停在第 %d 项「认输」", ui_state_cursor(&r.st));

    /* 再进一次，这次选「认输」—— 才真的吐动作 */
    got = tap(&r, KEY_OK, &out);
    check(!got && ui_state_screen(&r.st) == UI_SCREEN_CONFIRM, "再次进确认界面");
    ev_(&r, KEY_DOWN, KEY_EV_PRESS, NULL);
    check(ui_state_cursor(&r.st) == UI_CONFIRM_YES, "移到第 %d 项「认输」",
          ui_state_cursor(&r.st));
    got = tap(&r, KEY_OK, &out);
    check(got && out.act == UI_ACT_FORFEIT, "确认认输 → 吐 UI_ACT_FORFEIT（%s）",
          action_name(out.act));
    check(r.nactions == before + 1, "全程只多出这一个动作（动作数 %d）", r.nactions);
    check(ui_state_screen(&r.st) == UI_SCREEN_CONFIRM,
          "吐完动作界面不动 —— 结束对局由调用方做（置 BATTLE_LOSE 后 show_result）");

    /* 确认界面长按 = 返回上一层，是弹层不是战斗界面 */
    hold(&r, KEY_OK);
    check(ui_state_screen(&r.st) == UI_SCREEN_ACTION && ui_state_depth(&r.st) == 3,
          "确认界面长按 → 回弹层（栈深 %d）", ui_state_depth(&r.st));
    hold(&r, KEY_OK);
    check(ui_state_screen(&r.st) == UI_SCREEN_BATTLE && ui_state_depth(&r.st) == 2,
          "再长按 → 回战斗界面（栈深 %d，主菜单还压在下面）", ui_state_depth(&r.st));

    /* 确认界面里不存在「没有退路」的情况：它不是强制界面 */
    check(!ui_state_forced(&r.st), "战斗界面不是强制态");

    /* 反复进出确认界面不会让栈溢出 */
    Rig r2;
    rig_init(&r2);
    tap(&r2, KEY_OK, NULL);
    for (int i = 0; i < 4; i++) {
        hold(&r2, KEY_OK);                       /* 打开弹层 */
        ev_(&r2, KEY_DOWN, KEY_EV_PRESS, NULL);  /* 移到「认输」 */
        tap(&r2, KEY_OK, NULL);                  /* 进确认界面 */
        hold(&r2, KEY_OK);                       /* 退出确认界面 */
        hold(&r2, KEY_OK);                       /* 退出弹层 */
    }
    check(ui_state_screen(&r2.st) == UI_SCREEN_BATTLE
          && ui_state_depth(&r2.st) <= UI_STACK_MAX,
          "来回进出 4 轮不会让栈溢出（栈深 %d）", ui_state_depth(&r2.st));
    check(r2.nactions == 1, "这 4 轮里只产生过「开始对战」一个动作（动作数 %d）",
          r2.nactions);
}

static void test_forced_switch(void)
{
    printf("\n[强制换人] 出战濒死：没有退路\n");
    Rig r;
    rig_init(&r);
    const uint8_t slots[UI_TEAM_SLOTS] = { 1, 3, 5 };
    ui_state_force_switch(&r.st, slots, 3);

    check(ui_state_screen(&r.st) == UI_SCREEN_SWITCH, "直接进换人界面（%s）",
          screen_name(ui_state_screen(&r.st)));
    check(ui_state_forced(&r.st), "标记为强制");
    check(ui_state_count(&r.st) == 3, "只列 %d 只可选", ui_state_count(&r.st));

    hold(&r, KEY_OK);
    check(ui_state_screen(&r.st) == UI_SCREEN_SWITCH && ui_state_depth(&r.st) == 1,
          "长按被无视，仍在换人界面（栈深 %d）", ui_state_depth(&r.st));

    UiResult out;
    ev_(&r, KEY_DOWN, KEY_EV_PRESS, NULL);
    bool got = tap(&r, KEY_OK, &out);
    check(got && out.act == UI_ACT_SWITCH_MON && out.arg == 3,
          "换人列表第 2 项 → 队伍槽 3（arg=%d）", out.arg);
    check(ui_state_screen(&r.st) == UI_SCREEN_BATTLE && !ui_state_forced(&r.st),
          "换完回战斗界面且不再是强制态");

    /* 强制换人但列表为空（理论上不该发生）也不能崩，且不给动作 */
    Rig r2;
    rig_init(&r2);
    ui_state_force_switch(&r2.st, NULL, 0);
    check(ui_state_count(&r2.st) == 0, "空列表项数 0");
    got = tap(&r2, KEY_OK, &out);
    check(!got && ui_state_screen(&r2.st) == UI_SCREEN_SWITCH,
          "空列表上确定不报动作、也不换界面");
}

static void test_result(void)
{
    printf("\n[结算]\n");
    Rig r;
    rig_init(&r);
    ui_state_show_result(&r.st, 1);
    check(ui_state_screen(&r.st) == UI_SCREEN_RESULT, "进结算界面（%s）",
          screen_name(ui_state_screen(&r.st)));
    check(ui_state_result(&r.st) == 1, "结果码带上来了（%d）", ui_state_result(&r.st));

    hold(&r, KEY_OK);
    check(ui_state_screen(&r.st) == UI_SCREEN_MAIN, "长按 → 回主菜单（%s）",
          screen_name(ui_state_screen(&r.st)));

    ui_state_show_result(&r.st, 2);
    UiResult out;
    bool got = tap(&r, KEY_OK, &out);
    check(ui_state_screen(&r.st) == UI_SCREEN_MAIN && !got,
          "结算界面上确定 → 回主菜单，不报动作");
}

static void test_swallow_click(void)
{
    printf("\n[取键规则] 长按之后紧随的 CLICK 必须吞掉\n");

    /* 模拟官方按键组件那种「报过 LONG 又补一条 CLICK」的上游。
     * 我们自己的驱动不会这么发，但这一层不该指望上游。 */
    Rig r;
    rig_init(&r);
    ev_(&r, KEY_OK, KEY_EV_PRESS, NULL);
    ev_(&r, KEY_OK, KEY_EV_LONG, NULL);
    check(ui_state_screen(&r.st) == UI_SCREEN_MAIN && r.nactions == 0,
          "主菜单长按无效，仍在主菜单");
    ev_(&r, KEY_OK, KEY_EV_CLICK, NULL);
    check(r.nactions == 0, "紧随的那条 CLICK 被吞掉，没产生动作（动作数 %d）",
          r.nactions);
    ev_(&r, KEY_OK, KEY_EV_RELEASE, NULL);
    UiResult out;
    bool got = tap(&r, KEY_OK, &out);
    check(got && out.act == UI_ACT_START_BATTLE,
          "下一次轻点恢复正常：开始对战（%s）", action_name(out.act));

    /* 关键场景：在战斗界面上长按打开弹层，上游多报的那条 CLICK
     * 若不被吞掉，就会顺手把弹层第 1 项「换人」也确定掉。 */
    Rig r2;
    rig_init(&r2);
    tap(&r2, KEY_OK, NULL);                  /* 开始对战 */
    ev_(&r2, KEY_OK, KEY_EV_PRESS, NULL);
    ev_(&r2, KEY_OK, KEY_EV_LONG, NULL);
    check(ui_state_screen(&r2.st) == UI_SCREEN_ACTION, "长按 → 进弹层");
    int before = r2.nactions;
    ev_(&r2, KEY_OK, KEY_EV_CLICK, NULL);
    check(r2.nactions == before && ui_state_screen(&r2.st) == UI_SCREEN_ACTION,
          "紧随的 CLICK 被吞掉：没有误进换人界面（%s）",
          screen_name(ui_state_screen(&r2.st)));
    ev_(&r2, KEY_OK, KEY_EV_RELEASE, NULL);
}

static void test_texts(void)
{
    printf("\n[文字] 绘制层与自检取同一份\n");
    bool ok = true;
    for (int i = 0; i < UI_SCREEN_COUNT; i++) {
        const char *t = ui_screen_title((UiScreen)i);
        const char *h = ui_screen_hint((UiScreen)i);
        if (!t || !*t || !h || !*h) {
            ok = false;
        }
    }
    check(ok, "每个界面都有标题与键位提示");
    check(strcmp(ui_item_label(UI_SCREEN_MAIN, UI_MAIN_BATTLE), "开始对战") == 0,
          "主菜单第 1 项 = 开始对战");
    check(strcmp(ui_item_label(UI_SCREEN_MAIN, UI_MAIN_TEAM), "队伍配置") == 0,
          "主菜单第 2 项 = 队伍配置");
    check(strcmp(ui_item_label(UI_SCREEN_ACTION, UI_ACTION_SWITCH), "换人") == 0,
          "弹层第 1 项 = 换人");
    check(strcmp(ui_item_label(UI_SCREEN_ACTION, UI_ACTION_FORFEIT), "认输") == 0,
          "弹层第 2 项 = 认输");
    check(strcmp(ui_screen_title(UI_SCREEN_CONFIRM), "确定认输") == 0,
          "确认界面标题 = 确定认输");
    check(strcmp(ui_item_label(UI_SCREEN_CONFIRM, UI_CONFIRM_CANCEL), "取消") == 0,
          "确认界面第 1 项 = 取消（安全的那项排在前）");
    check(strcmp(ui_item_label(UI_SCREEN_CONFIRM, UI_CONFIRM_YES), "认输") == 0,
          "确认界面第 2 项 = 认输");
    check(ui_item_label(UI_SCREEN_CONFIRM, 99) == NULL, "确认界面越界取标签返回 NULL");
    check(ui_item_label(UI_SCREEN_TEAM, 0) == NULL,
          "列表类界面的行文字来自数据，不在这里写死");
    check(ui_item_label(UI_SCREEN_MAIN, 99) == NULL, "越界取标签返回 NULL");
    check(ui_screen_rows(UI_SCREEN_CONFIRM) == 2, "确认界面一屏 2 行");
    check(ui_screen_rows(UI_SCREEN_TEAM) == 8, "队伍列表一屏 8 行");
    check(ui_screen_rows(UI_SCREEN_BATTLE) == 2, "招式菜单是 2x2，占两行");
    check(ui_screen_rows(UI_SCREEN_SWITCH) == 6, "换人列表一屏 6 行");
}

static void test_defensive(void)
{
    printf("\n[防御性]\n");
    ui_state_init(NULL);
    ui_state_set_model(NULL, NULL);
    ui_state_force_switch(NULL, NULL, 3);
    ui_state_show_result(NULL, 1);
    check(!ui_state_feed(NULL, NULL, 0, NULL), "空指针喂事件返回假");
    check(!ui_state_tick(NULL, 0, NULL), "空指针 tick 返回假");
    check(ui_state_screen(NULL) == UI_SCREEN_MAIN, "空指针查界面返回主菜单");
    check(ui_state_cursor(NULL) == 0 && ui_state_count(NULL) == 0
          && ui_state_depth(NULL) == 0, "空指针查光标/项数/栈深都返回 0");

    /* 调用方不关心动作时传 NULL 也不能崩 */
    Rig r;
    rig_init(&r);
    KeyMsg m = { .key = KEY_OK, .ev = KEY_EV_CLICK, .mv = 0, .t_ms = 16 };
    check(ui_state_feed(&r.st, &m, 16, NULL), "UiResult 传 NULL 时仍能工作");

    /* 反复切换不会让栈溢出 */
    Rig r2;
    rig_init(&r2);
    tap(&r2, KEY_OK, NULL);
    for (int i = 0; i < 6; i++) {
        hold(&r2, KEY_OK);
    }
    check(ui_state_depth(&r2.st) <= UI_STACK_MAX, "反复切换不会让栈溢出（栈深 %d）",
          ui_state_depth(&r2.st));

    /* 非法的 species_count 要回退到满列表，不能算出负的项数 */
    Rig r3;
    rig_init(&r3);
    UiModel neg;
    memset(&neg, 0, sizeof(neg));
    neg.species_count = -5;
    neg.move_count = 4;
    ui_state_set_model(&r3.st, &neg);
    ev_(&r3, KEY_DOWN, KEY_EV_PRESS, NULL);
    tap(&r3, KEY_OK, NULL);
    check(ui_state_screen(&r3.st) == UI_SCREEN_TEAM
          && ui_state_count(&r3.st) == UI_TEAM_COUNT,
          "非法 species_count(-5) → 回退到 %d 项", ui_state_count(&r3.st));
}

/* ================================================================ 回放 */

static void test_trace(void)
{
    printf("\n[序列回放] 主菜单 → 战斗 → 弹层 → 换人 / 认输确认 → 长按退出\n");
    printf("  （这条轨迹也是本层的用法示例：每 16 ms 喂一次事件、tick 一次）\n");

    Rig r;
    rig_init(&r);
    UiModel m = model_sample();
    ui_state_set_model(&r.st, &m);

    static const struct { KeyId k; KeyEvent e; const char *what; } seq[] = {
        { KEY_DOWN, KEY_EV_PRESS,   "▲▼ 移到「队伍配置」" },
        { KEY_UP,   KEY_EV_PRESS,   "▲▼ 移回「开始对战」" },
        { KEY_OK,   KEY_EV_PRESS,   "OK 按下" },
        { KEY_OK,   KEY_EV_CLICK,   "OK 轻点 → 开始对战" },
        { KEY_OK,   KEY_EV_RELEASE, "OK 抬起" },
        { KEY_DOWN, KEY_EV_PRESS,   "▲▼ 移到第 2 个招式" },
        { KEY_OK,   KEY_EV_PRESS,   "OK 按下" },
        { KEY_OK,   KEY_EV_LONG,    "OK 长按 → 打开弹层（不会误出招）" },
        { KEY_OK,   KEY_EV_RELEASE, "OK 抬起" },
        { KEY_OK,   KEY_EV_PRESS,   "OK 按下" },
        { KEY_OK,   KEY_EV_CLICK,   "OK 轻点 → 进入换人列表" },
        { KEY_OK,   KEY_EV_RELEASE, "OK 抬起" },
        { KEY_DOWN, KEY_EV_PRESS,   "▲▼ 移到第 2 只" },
        { KEY_OK,   KEY_EV_PRESS,   "OK 按下" },
        { KEY_OK,   KEY_EV_CLICK,   "OK 轻点 → 换人（消耗回合）" },
        { KEY_OK,   KEY_EV_RELEASE, "OK 抬起" },
        { KEY_OK,   KEY_EV_PRESS,   "OK 按下" },
        { KEY_OK,   KEY_EV_LONG,    "OK 长按 → 打开弹层" },
        { KEY_OK,   KEY_EV_RELEASE, "OK 抬起" },
        { KEY_OK,   KEY_EV_PRESS,   "OK 按下" },
        { KEY_OK,   KEY_EV_LONG,    "OK 长按 → 返回战斗界面" },
        { KEY_OK,   KEY_EV_RELEASE, "OK 抬起" },
        /* 认输这条路：弹层 → 确认界面 → 退出。全程不产生动作。 */
        { KEY_OK,   KEY_EV_PRESS,   "OK 按下" },
        { KEY_OK,   KEY_EV_LONG,    "OK 长按 → 打开弹层" },
        { KEY_OK,   KEY_EV_RELEASE, "OK 抬起" },
        { KEY_DOWN, KEY_EV_PRESS,   "▲▼ 移到「认输」" },
        { KEY_OK,   KEY_EV_PRESS,   "OK 按下" },
        { KEY_OK,   KEY_EV_CLICK,   "OK 轻点 → 进认输确认（还不算判负）" },
        { KEY_OK,   KEY_EV_RELEASE, "OK 抬起" },
        { KEY_OK,   KEY_EV_PRESS,   "OK 按下" },
        { KEY_OK,   KEY_EV_LONG,    "OK 长按 → 退出确认界面" },
        { KEY_OK,   KEY_EV_RELEASE, "OK 抬起" },
        { KEY_OK,   KEY_EV_PRESS,   "OK 按下" },
        { KEY_OK,   KEY_EV_LONG,    "OK 长按 → 返回战斗界面" },
        { KEY_OK,   KEY_EV_RELEASE, "OK 抬起" },
    };

    for (size_t i = 0; i < sizeof(seq) / sizeof(seq[0]); i++) {
        UiResult out;
        bool got = ev_(&r, seq[i].k, seq[i].e, &out);
        printf("    t=%4ums  %-4s %-7s  %-30s → %-8s 光标=%d %s%s\n",
               (unsigned)r.t, key_name(seq[i].k),
               seq[i].e == KEY_EV_PRESS ? "PRESS" :
               seq[i].e == KEY_EV_CLICK ? "CLICK" :
               seq[i].e == KEY_EV_LONG  ? "LONG" : "RELEASE",
               seq[i].what, screen_name(ui_state_screen(&r.st)),
               ui_state_cursor(&r.st),
               got ? "动作=" : "", got ? action_name(out.act) : "");
    }

    check(ui_state_screen(&r.st) == UI_SCREEN_BATTLE,
          "回放结束：停在战斗主界面（%s）", screen_name(ui_state_screen(&r.st)));
    check(r.nactions == 2, "整串只产生 2 个动作（开始对战、换人），实际 %d", r.nactions);
    check(r.last.act == UI_ACT_SWITCH_MON && r.last.arg == 2,
          "最后一个动作是换到队伍槽 %d（%s）", r.last.arg, action_name(r.last.act));
}

int main(void)
{
    printf("界面状态机自检\n");
    printf("==============\n");

    test_init();
    test_main_nav();
    test_team_scroll();
    test_battle_moves();
    test_overlay();
    test_forfeit_confirm();
    test_forced_switch();
    test_result();
    test_swallow_click();
    test_texts();
    test_defensive();
    test_trace();

    printf("\n=== 结果: %d 通过, %d 失败 ===\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}

/* ================================================================ 名字 */

const char *screen_name(UiScreen sc)
{
    switch (sc) {
    case UI_SCREEN_MAIN:    return "主菜单";
    case UI_SCREEN_TEAM:    return "队伍配置";
    case UI_SCREEN_BATTLE:  return "战斗界面";
    case UI_SCREEN_ACTION:  return "弹层";
    case UI_SCREEN_CONFIRM: return "认输确认";
    case UI_SCREEN_SWITCH:  return "换人";
    case UI_SCREEN_RESULT:  return "结算";
    default:                return "?";
    }
}

const char *action_name(UiAction a)
{
    switch (a) {
    case UI_ACT_NONE:          return "无";
    case UI_ACT_START_BATTLE:  return "开始对战";
    case UI_ACT_PICK_SPECIES:  return "选图鉴";
    case UI_ACT_CHOOSE_MOVE:   return "出招";
    case UI_ACT_SWITCH_MON:    return "换人";
    case UI_ACT_FORFEIT:       return "认输";
    default:                   return "?";
    }
}
