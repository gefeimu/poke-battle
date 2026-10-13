/*
 * test_session.c - 会话层自检（路线图第 3 项「把状态机接到引擎」）
 *
 * 这一层接的线最多，所以它出的一类 bug 也最隐蔽：**画面没坏、断言也没崩，
 * 只是回合流程少了（或多了）一步**。典型的几种：
 *
 *   - 引擎一次写 5 条日志，界面只放 1 条就把控制权还给玩家 —— 后半段战斗
 *     过程用户永远看不到；
 *   - 出战那只倒下后走错分支，让 battle_step 去执行换人（引擎会拒绝推进
 *     回合），于是「按了没反应」；
 *   - 日志播放游标用环形缓冲下标而不是绝对序号，日志一旦超过 24 条开始
 *     覆盖，游标就会错位、重播或跳播。
 *
 * 这些只有「喂一串按键把整局打完」才测得出来，所以这里是唯一一个把引擎、
 * AI、状态机、字库全链起来跑的测试。对手是 AI，所以要像 test_battle 那样
 * 覆盖随机源，否则结果不可重放。
 */
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "poke_font_data.h"
#include "poke_session.h"

/* ================================================================ 固定随机源 */

static uint32_t s_rng = 0x12345678u;

uint32_t battle_rng(void)
{
    s_rng ^= s_rng << 13;
    s_rng ^= s_rng >> 17;
    s_rng ^= s_rng << 5;
    return s_rng;
}

/* 伤害随机因子同源覆盖（引擎内的实现被 -DPOKE_CALC_TEST_NO_RAND 关掉了） */
uint8_t calc_rand_85_100(void)
{
    s_rng ^= s_rng << 13;
    s_rng ^= s_rng >> 17;
    s_rng ^= s_rng << 5;
    return (uint8_t)(85 + (s_rng % 16));
}

static void rng_reset(void) { s_rng = 0x12345678u; }

/* ================================================================ 断言 */

static int g_pass = 0, g_fail = 0;

static void check(int cond, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    char buf[260];
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

/* 折行要量真实像素宽，所以用固件里那三套字（与 dump_screen 一致） */
static const UiFonts kFonts = {
    .box = &kFontApp12,
    .dialog = &kFontApp16,
    .label = &kFontHp10,
};

static const char *phase_name(SesPhase p)
{
    switch (p) {
    case SES_PHASE_MAIN:   return "主菜单";
    case SES_PHASE_LOG:    return "日志播放";
    case SES_PHASE_INPUT:  return "等输入";
    case SES_PHASE_RESULT: return "结算";
    default:               return "?";
    }
}

static const char *screen_name(UiScreen sc)
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

/* ================================================================ 试验台 */

typedef struct {
    Session  s;
    uint32_t t;
} Rig;

static void rig_init(Rig *r, uint8_t level)
{
    rng_reset();
    ses_init(&r->s, level);
    ses_set_fonts(&r->s, &kFonts);
    r->t = 0;
}

static void ev(Rig *r, KeyId k, KeyEvent e)
{
    r->t += 16;
    KeyMsg m = { .key = k, .ev = e, .mv = 0, .t_ms = r->t };
    ses_feed(&r->s, &m, r->t);
}

/* 一次轻点：按下 → 短按 → 抬起 */
static void tap(Rig *r, KeyId k)
{
    ev(r, k, KEY_EV_PRESS);
    ev(r, k, KEY_EV_CLICK);
    ev(r, k, KEY_EV_RELEASE);
}

/* 一次长按：按下 → 长按 → 抬起 */
static void hold(Rig *r, KeyId k)
{
    ev(r, k, KEY_EV_PRESS);
    ev(r, k, KEY_EV_LONG);
    ev(r, k, KEY_EV_RELEASE);
}

static void ok_(Rig *r)   { tap(r, KEY_OK); }
static void up_(Rig *r)   { tap(r, KEY_UP); }
static void down_(Rig *r) { tap(r, KEY_DOWN); }

/*
 * 取列表第 i 行并与期望值比较。
 *
 * 不走这个包装直接 `strcmp(ses_row_text(...), "...")` 会**段错误**而不是
 * 断言失败：ses_row_text 在「界面不是列表」或「越界」时返回 NULL，而
 * UCRT 的 strcmp 对 NULL 直接崩。曾经就是这么崩的 —— 排查花了很久，
 * 因为崩溃点在 strcmp 里，看不出真正的问题是「界面压根不是预期的那个」。
 */
static void check_row(const Rig *r, int i, const char *want)
{
    const char *got = ses_row_text(&r->s, i);
    check(got && strcmp(got, want) == 0, "第 %d 行 = %s（实际 %s）",
          i + 1, want, got ? got : "(null)");
}

/*
 * 独立实现的 UTF-8 合法性检查。
 *
 * 不能拿 font_utf8_next 去验证 font_wrap2 的输出 —— 用被测实现的游标去验
 * 被测实现的产物，等于没验（上游游标遇到半个字也照样往前挪）。这里按字节
 * 自己走一遍编码规则：前导字节决定后续几个续接字节，续接字节必须是 10xxxxxx。
 */
static bool utf8_valid(const char *s)
{
    const unsigned char *p = (const unsigned char *)s;
    while (*p) {
        int extra;
        if (*p < 0x80) {
            extra = 0;
        } else if ((*p & 0xE0) == 0xC0) {
            extra = 1;
        } else if ((*p & 0xF0) == 0xE0) {
            extra = 2;
        } else if ((*p & 0xF8) == 0xF0) {
            extra = 3;
        } else {
            return false;   /* 0x80..0xBF 或 0xF8..0xFF 不可能是前导字节 */
        }
        for (int i = 0; i < extra; i++) {
            if ((p[1 + i] & 0xC0) != 0x80) {
                return false;
            }
        }
        p += 1 + extra;
    }
    return true;
}

/* 一路按 OK 把当前这段日志播完，返回按了几次 */
static int play_log(Rig *r)
{
    int n = 0;
    while (ses_phase(&r->s) == SES_PHASE_LOG && n < 400) {
        ok_(r);
        n++;
    }
    return n;
}

/* 从主菜单开打，并把开场日志播完（停在等输入） */
static void start_battle(Rig *r)
{
    ok_(r);            /* 主菜单第 1 项：开始对战 */
    play_log(r);
}

/* 在所有日志里找一句包含 needle 的，返回绝对序号（找不到返回 -1） */
static int find_log(const Session *s, const char *needle)
{
    for (uint32_t i = 0; i < s->b.log.count; i++) {
        const char *line = ses_log_at(s, i);
        if (line && strstr(line, needle)) {
            return (int)i;
        }
    }
    return -1;
}

/* ================================================================ 用例 */

static void test_init(void)
{
    printf("\n[初始化]\n");
    Rig r;
    rig_init(&r, AI_ROOKIE);

    check(ses_phase(&r.s) == SES_PHASE_MAIN, "初始在主菜单（%s）",
          phase_name(ses_phase(&r.s)));
    check(ui_state_screen(&r.s.ui) == UI_SCREEN_MAIN, "状态机也在主菜单（%s）",
          screen_name(ui_state_screen(&r.s.ui)));
    check(r.s.b.result == BATTLE_ONGOING, "对局还没开始（result = %u）",
          r.s.b.result);
    check(r.s.b.log.count == 0, "日志是空的（%u 条）", r.s.b.log.count);

    const BattleMon *me = battle_active_c(&r.s.b, 0);
    const BattleMon *foe = battle_active_c(&r.s.b, 1);
    check(me && foe, "双方都装好了出战宝可梦");
    check(me && me->cur_hp == me->hp_max, "我方满血（%u/%u）",
          me ? me->cur_hp : 0, me ? me->hp_max : 0);
    check(me && me->move[0] && me->pp[0] == me->pp_max[0], "招式与 PP 都就位");
    check_row(&r, 0, "开始对战");
    check_row(&r, 1, "队伍配置");
    check(ses_row_text(&r.s, 2) == NULL, "越界取行返回 NULL");
    check(strcmp(ses_result_text(&r.s), "") == 0, "还没结算，结算文字为空");
}

static void test_intro(void)
{
    printf("\n[开场] 开始对战 → 逐条播开场日志\n");
    Rig r;
    rig_init(&r, AI_ROOKIE);
    ok_(&r);

    check(ses_phase(&r.s) == SES_PHASE_LOG, "进入日志播放（%s）",
          phase_name(ses_phase(&r.s)));
    check(ui_state_screen(&r.s.ui) == UI_SCREEN_BATTLE, "底下已经是战斗界面（%s）",
          screen_name(ui_state_screen(&r.s.ui)));
    check(r.s.b.turn == 1, "回合数从 1 起（%u）", r.s.b.turn);
    check(r.s.b.log.count == 3, "开场写了 3 条日志（%u）", r.s.b.log.count);
    check(r.s.log_end == 3, "播放终点 = 3（log_end = %u）", r.s.log_end);

    /* 逐条播：按一次 OK 前进一条，不多不少 */
    for (uint32_t i = 0; i < 3; i++) {
        check(ses_log_index(&r.s) == i, "第 %u 次 OK 前停在第 %u 条", i + 1, i);
        ok_(&r);
    }
    check(ses_phase(&r.s) == SES_PHASE_INPUT, "3 条播完，交给玩家（%s）",
          phase_name(ses_phase(&r.s)));
    check(r.s.log_advances == 3, "统计到 3 次推进（%u）", r.s.log_advances);

    check(strcmp(ses_log_at(&r.s, 0), "战斗开始！我方 对 对手") == 0,
          "第 1 条 = 「%s」", ses_log_at(&r.s, 0));
    check(strcmp(ses_log_at(&r.s, 1), "我方 派出了 喷火龙！") == 0,
          "第 2 条 = 「%s」", ses_log_at(&r.s, 1));
    check(strcmp(ses_log_at(&r.s, 2), "对手 派出了 巨金怪！") == 0,
          "第 3 条 = 「%s」", ses_log_at(&r.s, 2));
    check(ses_log_at(&r.s, 3) == NULL, "越界取日志返回 NULL");
}

static void test_first_turn(void)
{
    printf("\n[一个回合] 出招 → 引擎推进 → 播放新日志\n");
    Rig r;
    rig_init(&r, AI_ROOKIE);
    start_battle(&r);

    BattleMon *me = battle_active(&r.s.b, 0);
    uint8_t  pp0 = me->pp[0];
    uint32_t n0 = r.s.b.log.count;

    ok_(&r);   /* 菜单第 1 项 → 招式槽 0 */

    check(ses_phase(&r.s) == SES_PHASE_LOG, "出招后进入日志播放（%s）",
          phase_name(ses_phase(&r.s)));
    check(r.s.b.turn == 2, "回合数推到 2（%u）", r.s.b.turn);
    check(me->pp[0] == pp0 - 1, "PP 减 1（%u → %u）", pp0, me->pp[0]);
    check(r.s.b.log.count > n0, "引擎写了新日志（%u → %u）",
          n0, r.s.b.log.count);
    check(r.s.log_next == n0, "播放游标正好从第 %u 条接上（%u）",
          n0, r.s.log_next);
    check(strcmp(ses_log_at(&r.s, n0), "--- 第 1 回合 ---") == 0,
          "第一条新日志=回合分隔（「%s」）", ses_log_at(&r.s, n0));

    /* 逐条播完，每一次 OK 只能前进一条 */
    uint32_t from = r.s.log_next, to = r.s.log_end;
    uint32_t n = 0;
    while (ses_phase(&r.s) == SES_PHASE_LOG && n < 400) {
        check(ses_log_index(&r.s) == from + n,
              "第 %u 次 OK 前停在第 %u 条（应 %u）", n + 1,
              ses_log_index(&r.s), from + n);
        ok_(&r);
        n++;
    }
    check(n == to - from, "按了 %u 次 OK，正好播完 %u 条（无重播/跳播）",
          n, to - from);
    check(ses_phase(&r.s) == SES_PHASE_INPUT, "播完回到等输入（%s）",
          phase_name(ses_phase(&r.s)));
}

static void test_menu_model(void)
{
    printf("\n[菜单模型] 序号 ≠ 招式槽；死掉/出战的那只不进换人列表\n");
    Rig r;
    rig_init(&r, AI_ROOKIE);
    start_battle(&r);

    BattleMon *me = battle_active(&r.s.b, 0);
    check(r.s.ui.model.move_count == 4, "四招都在菜单里（%d）",
          r.s.ui.model.move_count);

    /* 把槽 1 的 PP 耗光 */
    me->pp[1] = 0;
    ok_(&r);                       /* 出招（用的是槽 0） */
    play_log(&r);

    check(r.s.ui.model.move_count == 3, "PP 耗光的槽从菜单消失（剩 %d 项）",
          r.s.ui.model.move_count);
    check(r.s.ui.model.move_slot[0] == 0 && r.s.ui.model.move_slot[1] == 2 &&
          r.s.ui.model.move_slot[2] == 3,
          "菜单序号 → 招式槽 = {%u,%u,%u}（跳过了槽 1）",
          r.s.ui.model.move_slot[0], r.s.ui.model.move_slot[1],
          r.s.ui.model.move_slot[2]);

    /* 队伍里第 3 只倒下 → 换人列表少一项 */
    r.s.b.me.team[2].alive = false;
    r.s.b.me.team[2].cur_hp = 0;
    ok_(&r);
    play_log(&r);

    /* 期望值**从引擎现算**，不写死数字：这几回合里对手打死了谁是不确定的
     * （巨金怪的岩崩对喷火龙是 4 倍克制，两下就能带走）。写死 4 会让断言
     * 随随机源一变就假失败。 */
    int cnt = r.s.ui.model.switch_count;
    int alive_other = 0, dead_other = 0, has_dead = 0, has_active = 0;
    for (int i = 0; i < TEAM_SIZE; i++) {
        const BattleMon *t = &r.s.b.me.team[i];
        if (i == (int)r.s.b.me.active) {
            continue;
        }
        if (t->alive && t->cur_hp > 0) {
            alive_other++;
        } else {
            dead_other++;
        }
    }
    for (int i = 0; i < cnt; i++) {
        uint8_t slot = r.s.ui.model.switch_slot[i];
        if (slot >= TEAM_SIZE) {
            continue;
        }
        const BattleMon *t = &r.s.b.me.team[slot];
        if (!t->alive || t->cur_hp == 0) {
            has_dead = 1;
        }
        if (slot == (int)r.s.b.me.active) {
            has_active = 1;
        }
    }
    check(cnt == alive_other, "换人列表 = 除出战外还活着的 %d 只（实际 %d 项）",
          alive_other, cnt);
    check(dead_other >= 1, "队里确实有倒下的（%d 只，含我手动打倒的第 3 只）",
          dead_other);
    check(!has_dead, "倒下的那只不在换人列表里");
    check(!has_active, "当前出战的那只不在换人列表里");
    check(cnt == TEAM_SIZE - 1 - dead_other, "列表长度 = 5 − 倒下数（%d）", cnt);
}

static void test_voluntary_switch(void)
{
    printf("\n[换人] 长按开弹层 → 换人 → 消耗一个回合\n");
    Rig r;
    rig_init(&r, AI_ROOKIE);
    start_battle(&r);

    hold(&r, KEY_OK);
    check(ui_state_screen(&r.s.ui) == UI_SCREEN_ACTION, "长按开弹层（%s）",
          screen_name(ui_state_screen(&r.s.ui)));
    /* 栈深是 2 而不是 3：开始对战时 ses_begin 把界面栈**重置**成了「只有
     * 战斗界面」—— 战斗一旦开始就回不到主菜单，主菜单不再压在底下。 */
    check(ui_state_depth(&r.s.ui) == 2, "弹层压在战斗界面之上（栈深 %d）",
          ui_state_depth(&r.s.ui));
    check_row(&r, 0, "换人");
    check_row(&r, 1, "认输");

    ok_(&r);   /* 换人 → 换人列表 */
    check(ui_state_screen(&r.s.ui) == UI_SCREEN_SWITCH, "进入换人列表（%s）",
          screen_name(ui_state_screen(&r.s.ui)));
    check(!ui_state_forced(&r.s.ui), "自愿换人不是强制换人");
    check_row(&r, 0, "水箭龟");

    uint8_t turn0 = r.s.b.turn;
    ok_(&r);   /* 选第 1 项 */
    check(r.s.b.me.active == 1, "换上了队伍槽 1（active = %u）", r.s.b.me.active);
    check(r.s.b.turn == turn0 + 1, "换人消耗一个回合（%u → %u）",
          turn0, r.s.b.turn);
    check(ses_phase(&r.s) == SES_PHASE_LOG, "换人也写日志，接着播（%s）",
          phase_name(ses_phase(&r.s)));
    check(find_log(&r.s, "收回了") >= 0, "日志里有换人那句：%s",
          ses_log_at(&r.s, (uint32_t)find_log(&r.s, "收回了")));
    play_log(&r);
    check(ui_state_screen(&r.s.ui) == UI_SCREEN_BATTLE, "播完回到战斗界面（%s）",
          screen_name(ui_state_screen(&r.s.ui)));
}

static void test_forced_switch(void)
{
    printf("\n[强制换人] 出战倒下 → 必须先换人，长按无退路\n");
    Rig r;
    rig_init(&r, AI_ROOKIE);
    start_battle(&r);

    /* 让对手把出战那只打死是不可控的；直接按引擎语义把出战置为濒死，
     * 下一次 battle_step 的防呆护栏就会报「需要换人」。 */
    Battle *b = ses_battle(&r.s);
    uint8_t turn0 = b->turn;
    b->me.team[0].cur_hp = 0;
    b->me.team[0].alive = false;

    ok_(&r);   /* 选一招 —— 引擎会拒绝推进回合并报需要换人 */

    check(ses_phase(&r.s) == SES_PHASE_INPUT, "停在等输入，没被锁死（%s）",
          phase_name(ses_phase(&r.s)));
    check(ui_state_screen(&r.s.ui) == UI_SCREEN_SWITCH, "界面切到换人列表（%s）",
          screen_name(ui_state_screen(&r.s.ui)));
    check(ui_state_forced(&r.s.ui), "标记为强制换人");
    check(b->turn == turn0, "回合没有推进（护栏拒绝了 battle_step）（%u）",
          b->turn);
    check(ui_state_count(&r.s.ui) == 5, "只列剩下 5 只（%d）",
          ui_state_count(&r.s.ui));
    check_row(&r, 0, "水箭龟");

    hold(&r, KEY_OK);
    check(ui_state_screen(&r.s.ui) == UI_SCREEN_SWITCH,
          "强制换人时长按退不出去（%s）", screen_name(ui_state_screen(&r.s.ui)));

    ok_(&r);   /* 选第 1 只 */
    check(b->me.active == 1, "换上队伍槽 1（active = %u）", b->me.active);
    check(b->need_switch_me == 0, "换人需求已清（%u）", b->need_switch_me);
    check(b->turn == turn0, "强制换人本身不消耗回合（%u）", b->turn);
    check(ses_phase(&r.s) == SES_PHASE_LOG, "换人也写日志（%s）",
          phase_name(ses_phase(&r.s)));
    play_log(&r);
    check(ses_phase(&r.s) == SES_PHASE_INPUT, "播完回到等输入（%s）",
          phase_name(ses_phase(&r.s)));
    check(ui_state_screen(&r.s.ui) == UI_SCREEN_BATTLE, "并且回到战斗界面（%s）",
          screen_name(ui_state_screen(&r.s.ui)));
    check(!ui_state_forced(&r.s.ui), "强制标记已清除");

    /* 换完能继续正常出招 */
    uint8_t turn1 = b->turn;
    ok_(&r);
    check(b->turn == turn1 + 1, "换完人后能继续推进回合（%u → %u）",
          turn1, b->turn);
    play_log(&r);
}

static void test_struggle(void)
{
    printf("\n[PP 全空] 菜单给「挣扎」，不让玩家卡死\n");
    Rig r;
    rig_init(&r, AI_ROOKIE);
    start_battle(&r);

    BattleMon *me = battle_active(&r.s.b, 0);
    for (int i = 0; i < MOVES_PER_MON; i++) {
        me->pp[i] = 0;
    }

    /* 先把这一回合走完，让菜单模型跟着刷新 */
    ok_(&r);
    play_log(&r);

    check(r.s.struggle_menu, "进入挣扎模式");
    check(r.s.ui.model.move_count == 1, "菜单只剩 1 项（%d）",
          r.s.ui.model.move_count);

    BattleScreen v;
    ses_view(&r.s, &v);
    check(v.moves[0] && strcmp(v.moves[0], "挣扎") == 0,
          "菜单显示的是「%s」而不是槽里的招式名",
          v.moves[0] ? v.moves[0] : "(null)");
    check(v.move_pp[0] == NULL, "挣扎不显示 PP（它是无限的）");
    check(v.status_left && strcmp(v.status_left, "挣扎") == 0,
          "状态行左侧 = %s", v.status_left ? v.status_left : "(null)");

    uint8_t turn0 = r.s.b.turn;
    ok_(&r);
    check(r.s.b.turn == turn0 + 1, "PP 全空也能出招推进回合（%u → %u）",
          turn0, r.s.b.turn);
    check(find_log(&r.s, "挣扎") >= 0, "日志里使出了挣扎：%s",
          ses_log_at(&r.s, (uint32_t)find_log(&r.s, "挣扎")));
    play_log(&r);
}

static void test_forfeit(void)
{
    printf("\n[认输] 两步确认才判负\n");
    Rig r;
    rig_init(&r, AI_ROOKIE);
    start_battle(&r);

    uint8_t turn0 = r.s.b.turn;

    hold(&r, KEY_OK);          /* 弹层 */
    down_(&r);                 /* 光标到第 2 项「认输」 */
    check(ui_state_cursor(&r.s.ui) == 1, "光标在第 2 项（%d）",
          ui_state_cursor(&r.s.ui));
    ok_(&r);                   /* 进确认界面 */
    check(ui_state_screen(&r.s.ui) == UI_SCREEN_CONFIRM, "进入认输确认（%s）",
          screen_name(ui_state_screen(&r.s.ui)));
    check(r.s.b.result == BATTLE_ONGOING, "进确认界面本身不结算（result = %u）",
          r.s.b.result);
    check(ses_phase(&r.s) == SES_PHASE_INPUT, "阶段仍是等输入（%s）",
          phase_name(ses_phase(&r.s)));
    check_row(&r, 0, "取消");
    check_row(&r, 1, "认输");

    ok_(&r);                   /* 默认光标在「取消」→ 退回 */
    check(ui_state_screen(&r.s.ui) == UI_SCREEN_ACTION, "取消退回弹层（%s）",
          screen_name(ui_state_screen(&r.s.ui)));
    check(r.s.b.result == BATTLE_ONGOING, "取消没有判负（result = %u）",
          r.s.b.result);
    check(ui_state_cursor(&r.s.ui) == 1, "弹层光标还停在第 2 项（%d）",
          ui_state_cursor(&r.s.ui));

    ok_(&r);                   /* 再进确认 */
    down_(&r);                 /* 光标挪到「认输」 */
    ok_(&r);                   /* 确认判负 */
    check(r.s.b.result == BATTLE_LOSE, "判我方负（result = %u）", r.s.b.result);
    check(ses_phase(&r.s) == SES_PHASE_RESULT, "进入结算（%s）",
          phase_name(ses_phase(&r.s)));
    check(ui_state_screen(&r.s.ui) == UI_SCREEN_RESULT, "界面是结算（%s）",
          screen_name(ui_state_screen(&r.s.ui)));
    check(strcmp(ses_result_text(&r.s), "失败") == 0, "结算文字 = %s",
          ses_result_text(&r.s));
    check(r.s.b.turn == turn0, "认输不推进回合（弃权不是招式结算）（%u）",
          r.s.b.turn);
}

static void test_result_and_replay(void)
{
    printf("\n[结算后重开] 日志游标与队伍都要复位\n");
    Rig r;
    rig_init(&r, AI_ROOKIE);
    start_battle(&r);

    /* 快速认输 */
    hold(&r, KEY_OK);
    down_(&r);
    ok_(&r);
    down_(&r);
    ok_(&r);
    check(ses_phase(&r.s) == SES_PHASE_RESULT, "先走到结算（%s）",
          phase_name(ses_phase(&r.s)));

    ok_(&r);   /* 结算界面 OK → 主菜单 */
    check(ses_phase(&r.s) == SES_PHASE_MAIN, "回主菜单（%s）",
          phase_name(ses_phase(&r.s)));
    check(ui_state_screen(&r.s.ui) == UI_SCREEN_MAIN, "界面也回主菜单（%s）",
          screen_name(ui_state_screen(&r.s.ui)));
    check(r.s.b.result == BATTLE_ONGOING, "结果位复位（%u）", r.s.b.result);
    check(r.s.b.log.count == 0, "日志清空（%u 条）", r.s.b.log.count);
    check(r.s.log_next == 0 && r.s.log_end == 0, "播放游标复位（%u/%u）",
          r.s.log_next, r.s.log_end);

    const BattleMon *me = battle_active_c(&r.s.b, 0);
    check(me && me->cur_hp == me->hp_max, "我方队伍满血复位（%u/%u）",
          me ? me->cur_hp : 0, me ? me->hp_max : 0);
    check(me && me->pp[0] == me->pp_max[0], "PP 也复位（%u/%u）",
          me ? me->pp[0] : 0, me ? me->pp_max[0] : 0);
    check(strcmp(ses_result_text(&r.s), "") == 0, "结算文字已清空");

    /* 再来一局：游标必须从 0 开始，不能接着上一局的序号数下去 */
    ok_(&r);
    check(ses_phase(&r.s) == SES_PHASE_LOG, "第二局正常开局（%s）",
          phase_name(ses_phase(&r.s)));
    check(r.s.b.log.count == 3, "第二局的日志是新的 3 条（%u）",
          r.s.b.log.count);
    check(r.s.log_next == 0 && r.s.log_end == 3,
          "第二局的播放游标从 0 开始（%u/%u）", r.s.log_next, r.s.log_end);
    check(strcmp(ses_log_at(&r.s, 0), "战斗开始！我方 对 对手") == 0,
          "第二条第一条日志正常（「%s」）", ses_log_at(&r.s, 0));
}

static void test_log_skip_and_swallow(void)
{
    printf("\n[长按跳日志] 快进到末尾；紧随的 CLICK 要吞掉\n");
    Rig r;
    rig_init(&r, AI_ROOKIE);
    start_battle(&r);

    ok_(&r);   /* 出招 → 进日志播放 */
    check(ses_phase(&r.s) == SES_PHASE_LOG, "在播放日志（%s）",
          phase_name(ses_phase(&r.s)));
    uint8_t turn0 = r.s.b.turn;
    uint32_t end = r.s.log_end;

    hold(&r, KEY_OK);
    check(ses_phase(&r.s) == SES_PHASE_INPUT, "长按直接跳到末尾（%s）",
          phase_name(ses_phase(&r.s)));
    check(r.s.log_next == end, "游标停在终点（%u / %u）", r.s.log_next, end);
    check(r.s.log_skips == 1, "记了一次快进（%u）", r.s.log_skips);
    check(r.s.b.turn == turn0, "跳日志不会少算回合（%u）", r.s.b.turn);

    /* ---- 吞掉长按之后那次 CLICK ----
     * 官方按键组件某些配置会把 LONG 与 CLICK 都报出来；不挡的话
     * 「长按跳过日志」会顺手把战斗界面上光标那招也确定了。
     * 上游两种顺序都出现过，两种都要挡。 */
    ok_(&r);   /* 再出一次招，回到日志播放 */
    uint8_t turn1 = r.s.b.turn;
    ev(&r, KEY_OK, KEY_EV_LONG);     /* 只报长按 */
    check(ses_phase(&r.s) == SES_PHASE_INPUT, "只报长按也能跳过（%s）",
          phase_name(ses_phase(&r.s)));
    ev(&r, KEY_OK, KEY_EV_CLICK);    /* 紧随的短按应当被吞掉 */
    check(r.s.b.turn == turn1, "LONG → CLICK 顺序下，那次 CLICK 被吞掉（%u）",
          r.s.b.turn);
    ev(&r, KEY_OK, KEY_EV_RELEASE);

    ok_(&r);
    uint8_t turn2 = r.s.b.turn;
    ev(&r, KEY_OK, KEY_EV_LONG);
    ev(&r, KEY_OK, KEY_EV_RELEASE);
    ev(&r, KEY_OK, KEY_EV_CLICK);    /* 换一种顺序，同样要吞 */
    check(r.s.b.turn == turn2, "LONG → 抬起 → CLICK 顺序下也被吞掉（%u）",
          r.s.b.turn);

    /* 但下一次真正的按下不能被连坐 */
    ok_(&r);
    check(r.s.b.turn == turn2 + 1, "下一次正常轻点照常出招（%u → %u）",
          turn2, r.s.b.turn);
    play_log(&r);

    /* 结算界面上同样：长按返回主菜单后，紧随的 CLICK 不能顺手开始新对局 */
    hold(&r, KEY_OK);
    down_(&r);
    ok_(&r);
    down_(&r);
    ok_(&r);
    check(ses_phase(&r.s) == SES_PHASE_RESULT, "先到结算（%s）",
          phase_name(ses_phase(&r.s)));
    ev(&r, KEY_OK, KEY_EV_LONG);
    check(ses_phase(&r.s) == SES_PHASE_MAIN, "结算界面长按回主菜单（%s）",
          phase_name(ses_phase(&r.s)));
    ev(&r, KEY_OK, KEY_EV_CLICK);
    check(ses_phase(&r.s) == SES_PHASE_MAIN,
          "紧随的 CLICK 被吞掉，不会顺手开始新对局（%s）",
          phase_name(ses_phase(&r.s)));
}

static void test_view(void)
{
    printf("\n[一屏数据] BattleScreen 全部来自引擎\n");
    Rig r;
    rig_init(&r, AI_ROOKIE);
    start_battle(&r);

    const BattleMon *me = battle_active_c(&r.s.b, 0);

    BattleScreen v;
    ses_view(&r.s, &v);

    check(v.me_name && strcmp(v.me_name, "喷火龙") == 0, "我方名字 = %s",
          v.me_name ? v.me_name : "(null)");
    check(v.foe_name && strcmp(v.foe_name, "巨金怪") == 0, "对手名字 = %s",
          v.foe_name ? v.foe_name : "(null)");
    check(v.me_sprite != NULL && v.foe_sprite != NULL, "两边立绘都有");
    check(v.me_lv && strcmp(v.me_lv, "Lv50") == 0, "等级 = %s", v.me_lv);
    check(v.me_hp_permil == 1000 && v.foe_hp_permil == 1000,
          "满血时比例 = %d‰ / %d‰", v.me_hp_permil, v.foe_hp_permil);

    char want[32];
    snprintf(want, sizeof(want), "%u/%u", (unsigned)me->cur_hp,
             (unsigned)me->hp_max);
    check(v.me_hp_text && strcmp(v.me_hp_text, want) == 0,
          "HP 数字 = %s（应 %s）", v.me_hp_text ? v.me_hp_text : "(null)", want);

    /* 招式名与 PP 都要与引擎对得上，不能是写死的 */
    const MoveDef *mv0 = mv_get(me->move[r.s.ui.model.move_slot[0]]);
    check(v.moves[0] && mv0 && strcmp(v.moves[0], mv0->name_cn) == 0,
          "第 1 招 = %s（表里是 %s）", v.moves[0], mv0 ? mv0->name_cn : "?");
    snprintf(want, sizeof(want), "%u", (unsigned)me->pp[r.s.ui.model.move_slot[0]]);
    check(v.move_pp[0] && strcmp(v.move_pp[0], want) == 0, "第 1 招 PP = %s",
          v.move_pp[0]);
    check(v.moves[3] && v.move_pp[3], "4 个招式槽都填满了（第 4 项 = %s）",
          v.moves[3] ? v.moves[3] : "(null)");
    check(v.moves[3] && mv_get(me->move[r.s.ui.model.move_slot[3]])
           && strcmp(v.moves[3], mv_get(me->move[r.s.ui.model.move_slot[3]])->name_cn) == 0,
          "第 4 招也按菜单映射取，不是随手填的");

    check(v.selected == 0, "光标高亮第 1 项（%d）", v.selected);
    check(v.status_left && strncmp(v.status_left, "PP ", 3) == 0,
          "状态行左侧 = %s", v.status_left);
    check(v.status_right &&
          strcmp(v.status_right, ui_screen_hint(UI_SCREEN_BATTLE)) == 0,
          "状态行右侧 = 战斗界面的键位提示（%s）", v.status_right);
    check(v.log[0] != NULL, "对话框有文字（%s）", v.log[0] ? v.log[0] : "");

    /* 日志播放中不该有高亮 */
    ok_(&r);
    check(ses_phase(&r.s) == SES_PHASE_LOG, "进日志播放（%s）",
          phase_name(ses_phase(&r.s)));
    ses_view(&r.s, &v);
    check(v.selected == -1, "播放日志时不显示菜单高亮（%d）", v.selected);
    check(v.status_right && strcmp(v.status_right, "确定   长按跳过") == 0,
          "播放日志时右侧提示 = %s", v.status_right);

    /* 换人界面：行文字来自队伍数据。
     *
     * 但这里那一回合对手用岩崩（岩对火/飞 4 倍）足以把喷火龙直接打倒，
     * 界面进的是**强制换人**，行序行数都不是「6 只减出战」那套。所以先断言
     * 这条不变式（「出战倒下」与「界面进强制换人」必须同步），行内容则挪到
     * 一份没打过回合的干净存档上断言 —— 不把行号绑在随机结果上。 */
    play_log(&r);
    {
        bool down = (battle_active_c(&r.s.b, 0)->cur_hp == 0);
        check(ses_phase(&r.s) == SES_PHASE_INPUT, "回合收尾停在等输入（%s）",
              phase_name(ses_phase(&r.s)));
        check(ui_state_forced(&r.s.ui) == down,
              "出战%s，界面%s标记为强制换人（%s）",
              down ? "倒下" : "还站着", down ? "" : "不",
              screen_name(ui_state_screen(&r.s.ui)));
        if (down) {
            check(ui_state_screen(&r.s.ui) == UI_SCREEN_SWITCH,
                  "倒下就只能在换人列表里选（%s）",
                  screen_name(ui_state_screen(&r.s.ui)));
            check(r.s.b.need_switch_me == 1, "引擎挂起了我方换人请求（%d）",
                  r.s.b.need_switch_me);
        }
    }

    Rig clean;
    rig_init(&clean, AI_ROOKIE);
    start_battle(&clean);
    hold(&clean, KEY_OK);   /* 战斗界面长按 → 弹层 */
    ok_(&clean);            /* 弹层第 1 项：换人 */
    check(ui_state_screen(&clean.s.ui) == UI_SCREEN_SWITCH, "长按弹层进换人（%s）",
          screen_name(ui_state_screen(&clean.s.ui)));
    check(!ui_state_forced(&clean.s.ui), "自愿换人不是强制换人");
    check(ui_state_count(&clean.s.ui) == TEAM_SIZE - 1,
          "自愿换人列 6-1=5 项（%d）", ui_state_count(&clean.s.ui));
    check_row(&clean, 0, "水箭龟");
    check_row(&clean, 1, "妙蛙花");
    check_row(&clean, 2, "皮卡丘");
    check(ses_row_text(&clean.s, TEAM_SIZE - 1) == NULL, "越界取行返回 NULL");
}

static void test_team_screen(void)
{
    printf("\n[队伍配置] 列表行文字来自图鉴数据\n");
    Rig r;
    rig_init(&r, AI_ROOKIE);

    down_(&r);
    check(ui_state_cursor(&r.s.ui) == 1, "光标挪到第 2 项（%d）",
          ui_state_cursor(&r.s.ui));
    up_(&r);
    check(ui_state_cursor(&r.s.ui) == 0, "再按上回到第 1 项（%d）",
          ui_state_cursor(&r.s.ui));
    down_(&r);
    ok_(&r);
    check(ui_state_screen(&r.s.ui) == UI_SCREEN_TEAM, "进入队伍配置（%s）",
          screen_name(ui_state_screen(&r.s.ui)));
    check_row(&r, 0, "妙蛙种子");
    check_row(&r, 1, "妙蛙草");
    check_row(&r, 2, "妙蛙花");
    check(ui_state_count(&r.s.ui) == UI_TEAM_COUNT, "列表长度 = 图鉴只数（%d）",
          ui_state_count(&r.s.ui));
    check(ses_row_text(&r.s, UI_TEAM_COUNT) == NULL, "越界取行返回 NULL");
}

static void test_mega(void)
{
    printf("\n[Mega] 出招前进化，不消耗回合\n");
    Rig r;
    rig_init(&r, AI_ROOKIE);
    start_battle(&r);

    Battle *b = ses_battle(&r.s);
    BattleMon *me = battle_active(b, 0);
    me->base.item = 699;   /* Charizardite X */

    check(ses_player_mega(&r.s, 10034) == 0, "带对石头 → 进化成功");
    check(me->active_pid == 10034, "形态换成 Mega（pid = %u）", me->active_pid);
    check(me->mega_done == 1, "本场已用过 Mega（%u）", me->mega_done);
    check(ses_phase(&r.s) == SES_PHASE_LOG, "进化那句日志也要播（%s）",
          phase_name(ses_phase(&r.s)));
    check(find_log(&r.s, "超级进化了") >= 0, "日志里有：%s",
          ses_log_at(&r.s, (uint32_t)find_log(&r.s, "超级进化了")));
    play_log(&r);

    check(ses_player_mega(&r.s, 10034) == -1, "同一只不能再进化");
    check(ses_player_mega(&r.s, 10036) == -1, "不是这只的石头，拒绝");

    /* Mega 的立绘在 SPR_MEGA 那一档，用 SPR_FRONT 是查不到的 */
    BattleScreen v;
    ses_view(&r.s, &v);
    check(v.me_sprite != NULL, "Mega 形态也有立绘（贴图在 SPR_MEGA 档）");
    check(sprite_get(10034, SPR_FRONT) == NULL,
          "确认 SPR_FRONT 里没有 Mega 的图（所以必须按 is_mega 选档）");
}

static void test_device_path(void)
{
    printf("\n[设备通路] mV → 按键事件 → 会话\n");
    Rig r;
    rig_init(&r, AI_ROOKIE);

    /* 确定键落在 [447, 1900) 这个窗口里；松开态约 2959（实测值） */
    uint32_t t = 0;
    for (int i = 0; i < 60; i++) {
        ses_poll(&r.s, ++t, 1000);      /* 按住确定 */
    }
    check(ses_phase(&r.s) == SES_PHASE_MAIN, "按住还没抬起时不动（%s）",
          phase_name(ses_phase(&r.s)));
    for (int i = 0; i < 200; i++) {
        ses_poll(&r.s, ++t, 2959);      /* 松开 */
    }
    check(ses_phase(&r.s) == SES_PHASE_LOG,
          "整条通路能开局（mV → 事件 → 会话）（%s）",
          phase_name(ses_phase(&r.s)));

    /* 方向键：上 = [0,150)，下 = [150,447) */
    play_log(&r);
    hold(&r, KEY_OK);       /* 战斗界面长按 → 弹层 */
    down_(&r);              /* 光标到第 2 项「认输」 */
    ok_(&r);                /* 进认输确认 */
    ok_(&r);                /* 默认光标在「取消」→ 退回弹层 */
    /* 取消退回的是**弹层**，不是战斗界面 —— 弹层是压在战斗界面上的临时层，
     * 得再长按一次才收回去。这里曾经直接断言「回到战斗界面」，把两层混成了
     * 一层。 */
    check(ui_state_screen(&r.s.ui) == UI_SCREEN_ACTION, "取消后退回弹层（%s）",
          screen_name(ui_state_screen(&r.s.ui)));
    hold(&r, KEY_OK);       /* 弹层长按 → 收起弹层 */
    check(ui_state_screen(&r.s.ui) == UI_SCREEN_BATTLE, "弹层长按收回战斗界面（%s）",
          screen_name(ui_state_screen(&r.s.ui)));
    int c0 = ui_state_cursor(&r.s.ui);
    for (int i = 0; i < 60; i++) {
        ses_poll(&r.s, ++t, 300);       /* 下 */
    }
    for (int i = 0; i < 200; i++) {
        ses_poll(&r.s, ++t, 2959);
    }
    check(ui_state_cursor(&r.s.ui) != c0, "ADC 读数 300 mV 认作「下」，光标动了（%d → %d）",
          c0, ui_state_cursor(&r.s.ui));
}

static void test_hint_width(void)
{
    printf("\n[版式] 键位提示必须放得下\n");
    for (int i = 0; i < UI_SCREEN_COUNT; i++) {
        const char *h = ui_screen_hint((UiScreen)i);
        int w = font_text_width(&kFontApp16, h);
        check(w <= UI_STATUS_HINT_W, "%s 的提示 %d px ≤ %d px（「%s」）",
              screen_name((UiScreen)i), w, UI_STATUS_HINT_W, h);
    }
    /* 会话自己写的两条提示 */
    const char *ses_hints[] = { "确定   长按跳过", "确定继续", "挣扎" };
    for (unsigned i = 0; i < sizeof(ses_hints) / sizeof(*ses_hints); i++) {
        int w = font_text_width(&kFontApp16, ses_hints[i]);
        check(w <= UI_STATUS_HINT_W, "会话提示 %d px ≤ %d px（「%s」）",
              w, UI_STATUS_HINT_W, ses_hints[i]);
    }
}

/*
 * 状态行的提示除了「像素宽放得下」，还必须「字节数装得进缓冲」——
 * 两件事是独立的：提示可能画得下、却因为 Session 里的缓冲太小被 snprintf
 * 截断，而且截在半个汉字上。
 *
 * 这不是假想：status_right 原来定长 24，而最长的一条提示是 36 字节
 * （12 个汉字 + 2 个间隔），于是弹层/换人/认输确认三个界面的状态行
 * 右侧都被拦腰切断，尾巴上是半个汉字 —— 屏幕上是个乱码方块，
 * 肉眼不看这几个界面根本发现不了。
 */
static void test_status_hint_fit(void)
{
    printf("\n[状态行] 键位提示整条装得下\n");

    int worst = 0;
    const char *worst_s = "";
    for (int i = 0; i < UI_SCREEN_COUNT; i++) {
        const char *h = ui_screen_hint((UiScreen)i);
        int len = (int)strlen(h);
        if (len > worst) {
            worst = len;
            worst_s = h;
        }
        check(len < SES_STATUS_TEXT_MAX, "%s 的提示 %d 字节 < 缓冲 %d",
              screen_name((UiScreen)i), len, SES_STATUS_TEXT_MAX);
        check(utf8_valid(h), "%s 的提示本身是合法 UTF-8", screen_name((UiScreen)i));
    }
    /* 万一哪天提示全被改短，「最长 24 字节」之类的数字会失去意义，
     * 这条保证上面那圈断言确实覆盖到了长提示 */
    check(worst >= 30, "确实测到了长提示（最长 %d 字节：%s）", worst, worst_s);

    for (unsigned i = 0; i < 3; i++) {
        const char *h = (const char *[]){ "确定   长按跳过", "确定继续", "挣扎" }[i];
        check((int)strlen(h) < SES_STATUS_TEXT_MAX,
              "会话提示 %d 字节 < 缓冲 %d（「%s」）",
              (int)strlen(h), SES_STATUS_TEXT_MAX, h);
    }

    /* 真机上再过一遍：进弹层后状态行必须与提示表逐字节相同 */
    Rig r;
    rig_init(&r, AI_ROOKIE);
    start_battle(&r);
    hold(&r, KEY_OK);
    BattleScreen v;
    ses_view(&r.s, &v);
    const char *want = ui_screen_hint(UI_SCREEN_ACTION);
    check(v.status_right && strcmp(v.status_right, want) == 0,
          "弹层状态行逐字节等于提示表（实际 %s）",
          v.status_right ? v.status_right : "(null)");
}

static void test_wrap(void)
{
    printf("\n[折行] 对话框只有两行\n");
    char a[96], b[96], joined[192];

    int n = font_wrap2(&kFontApp16, "会心一击！", UI_DLG_TEXT_W,
                       a, sizeof(a), b, sizeof(b));
    check(n == 1 && strcmp(a, "会心一击！") == 0 && b[0] == '\0',
          "短句一行放得下（%d 行，「%s」）", n, a);

    /* 这是引擎真实会产生的最长的一条（收回并派出） */
    const char *long_one = "对手 收回了 巨金怪，派出了 沙奈朵！";
    check(font_text_width(&kFontApp16, long_one) > UI_DLG_TEXT_W,
          "这条确实超宽（%d px > %d px），必须折行",
          font_text_width(&kFontApp16, long_one), UI_DLG_TEXT_W);
    n = font_wrap2(&kFontApp16, long_one, UI_DLG_TEXT_W,
                   a, sizeof(a), b, sizeof(b));
    check(n == 2, "超宽句折成 2 行（%d）", n);
    snprintf(joined, sizeof(joined), "%s%s", a, b);
    check(strcmp(joined, long_one) == 0,
          "折行不丢字不重字（「%s」+「%s」）", a, b);
    check(font_text_width(&kFontApp16, a) <= UI_DLG_TEXT_W,
          "第一行不超宽（%d px）", font_text_width(&kFontApp16, a));
    check(font_text_width(&kFontApp16, b) <= UI_DLG_TEXT_W,
          "第二行不超宽（%d px）", font_text_width(&kFontApp16, b));
    check(a[0] != '\0' && b[0] != '\0', "折出来的两行都不是空的（「%s」/「%s」）",
          a, b);
    check(utf8_valid(a) && utf8_valid(b), "两行都是合法 UTF-8（没劈开多字节字）");

    /* 两行也放不下时要如实报出来（调用方据此知道这段文字塞不进这个框） */
    const char *too_long =
        "对手 的 巨金怪 使用了 彗星拳！会心一击！造成了 187 点伤害，"
        "巨金怪 的 攻击 大幅提高了！";
    n = font_wrap2(&kFontApp16, too_long, UI_DLG_TEXT_W,
                   a, sizeof(a), b, sizeof(b));
    check(n > 2, "两行放不下时返回 >2（%d）", n);
    /* 这一支的契约：第一行照常填，第二行起**全部**塞进 l1（超出就截断），
     * 所以 a+b 必须是原文的前缀，且仍然断在字符边界上。 */
    check(utf8_valid(a) && utf8_valid(b), "塞不下时两行仍是合法 UTF-8");
    snprintf(joined, sizeof(joined), "%s%s", a, b);
    check(strncmp(joined, too_long, strlen(joined)) == 0, "塞不下时不丢字不插字");

    /* 把宽度压到「一行只装得下 1 个字符」：断点必然落在多字节字符中间，
     * 这一条专盯「折出半个汉字」这个经典错误。 */
    n = font_wrap2(&kFontApp16, long_one, 1, a, sizeof(a), b, sizeof(b));
    check(utf8_valid(a) && utf8_valid(b), "极窄宽度下也不劈开多字节字符");
    check(a[0] != '\0' && strlen(a) <= 3, "第一行只放得下 1 个字符（%d 字节）",
          (int)strlen(a));

    /* 缓冲区装不下时在字符边界截断，不越界 */
    char small[8];
    n = font_wrap2(&kFontApp16, long_one, UI_DLG_TEXT_W,
                   small, sizeof(small), b, sizeof(b));
    check(strlen(small) < sizeof(small), "缓冲区小的时候不越界写（%d 字节）",
          (int)strlen(small));
}

/*
 * 菜单层的数据（路线图第 4 项）：ses_layer() 说该画哪几层，
 * ses_menu_view() 把状态机 + 队伍数据翻成 MenuScreen。
 *
 * 这里断的是**数据**；像素层面的版式在 test_render.c 里取点验证。
 * 两边分开的原因：数据错（比如把滚动窗口的首行填错了）画出来也是
 * 「有一行但内容不对」，取点验证抓不到内容，只能抓颜色。
 */
static void test_menu_view(void)
{
    printf("\n[菜单数据] ses_layer / ses_menu_view\n");
    Rig r;
    MenuScreen m;

    /* ---- 主菜单 ---- */
    rig_init(&r, AI_ROOKIE);
    check(ses_layer(&r.s) == SES_LAYER_MENU, "主菜单是整屏菜单层");
    check(ses_menu_view(&r.s, &m), "主菜单要画菜单");
    check(strcmp(m.title, "主菜单") == 0, "标题 = %s", m.title ? m.title : "(null)");
    check(m.big == NULL, "主菜单没有结算大字");
    check(m.count == 2 && m.rows == 2, "两项、一屏两行（实际 %d/%d）",
          m.count, m.rows);
    check(m.selected == 0 && m.top == 0, "光标在第 1 项");
    check(m.items[0] && strcmp(m.items[0], "开始对战") == 0, "第 1 项 = 开始对战");
    check(m.items[1] && strcmp(m.items[1], "队伍配置") == 0, "第 2 项 = 队伍配置");
    check(m.scrollbar == false, "两项不需要滚动条");
    check(m.status_left == NULL, "主菜单状态行左侧留空");
    check(m.status_right && strcmp(m.status_right, ui_screen_hint(UI_SCREEN_MAIN)) == 0,
          "状态行右侧 = 状态机的提示（%s）",
          m.status_right ? m.status_right : "(null)");

    down_(&r);
    check(ses_menu_view(&r.s, &m) && m.selected == 1, "▼ 之后光标到第 2 项");

    /* ---- 队伍配置：386 项 + 滚动窗口 + 计数 ---- */
    ok_(&r);
    check(ses_layer(&r.s) == SES_LAYER_MENU, "队伍配置是整屏菜单层");
    check(ses_menu_view(&r.s, &m), "队伍配置要画菜单");
    check(m.count == UI_TEAM_COUNT, "共 %d 项（图鉴只数）", m.count);
    check(m.rows == 8, "一屏 8 行（实际 %d）", m.rows);
    check(m.scrollbar == true, "386 项要滚动条");
    check(m.status_left && strcmp(m.status_left, "1/386") == 0,
          "状态行左侧报序号（%s）", m.status_left ? m.status_left : "(null)");
    check_row(&r, 0, "妙蛙种子");

    for (int i = 0; i < 12; i++) {
        down_(&r);
    }
    check(ses_menu_view(&r.s, &m), "滚动了 12 格仍能取到菜单");
    check(m.top == 5, "光标 12、一屏 8 行 → 窗口首行 = 5（实际 %d）", m.top);
    check(m.selected == 12, "光标 = 12");
    /* 窗口首行 = 图鉴第 6 只（列表第 i 项 = 图鉴序号 i+1） */
    const SpeciesDef *sd6 = species_get(6);
    check(m.items[0] && sd6 && strcmp(m.items[0], sd6->name_cn) == 0,
          "窗口首行是图鉴第 6 只（%s）",
          m.items[0] ? m.items[0] : "(null)");
    check(m.status_left && strcmp(m.status_left, "13/386") == 0,
          "计数跟着光标走（%s）", m.status_left ? m.status_left : "(null)");

    /* ---- 日志播放中：没有菜单层 ---- */
    rig_init(&r, AI_ROOKIE);
    start_battle(&r);
    check(ses_phase(&r.s) == SES_PHASE_INPUT, "已到等输入");
    check(ses_layer(&r.s) == SES_LAYER_BATTLE, "战斗界面不带菜单层");
    check(ses_menu_view(&r.s, &m) == false, "战斗界面不吐菜单数据");

    /* ---- 弹层 ---- */
    hold(&r, KEY_OK);
    check(ses_layer(&r.s) == SES_LAYER_BATTLE_MENU, "弹层 = 战斗界面 + 菜单层");
    check(ses_menu_view(&r.s, &m), "弹层要画菜单");
    check(strcmp(m.title, "选择") == 0, "弹层标题 = %s",
          m.title ? m.title : "(null)");
    check(m.count == 2 && m.rows == 2, "两项（实际 %d/%d）", m.count, m.rows);
    check(m.items[0] && strcmp(m.items[0], "换人") == 0, "第 1 项 = 换人");
    check(m.items[1] && strcmp(m.items[1], "认输") == 0, "第 2 项 = 认输");
    check(m.scrollbar == false, "弹层不画滚动条");

    /* ---- 认输确认：默认光标停在「取消」 ---- */
    down_(&r);
    ok_(&r);
    check(ses_layer(&r.s) == SES_LAYER_BATTLE_MENU, "确认界面也是弹层");
    check(ses_menu_view(&r.s, &m), "确认界面要画菜单");
    check(strcmp(m.title, "确定认输") == 0, "标题 = %s",
          m.title ? m.title : "(null)");
    check(m.selected == UI_CONFIRM_CANCEL, "默认光标停在「取消」（安全的那项）");
    check(m.items[0] && strcmp(m.items[0], "取消") == 0, "第 1 项 = 取消");
    check(m.items[1] && strcmp(m.items[1], "认输") == 0, "第 2 项 = 认输");

    /* ---- 换人：整屏列表 + 每行右侧的当前 HP ---- */
    rig_init(&r, AI_ROOKIE);
    start_battle(&r);
    hold(&r, KEY_OK);
    ok_(&r);
    check(ui_state_screen(&r.s.ui) == UI_SCREEN_SWITCH, "已到换人列表");
    check(ses_layer(&r.s) == SES_LAYER_MENU, "换人是整屏菜单层（不是弹层）");
    check(ses_menu_view(&r.s, &m), "换人要画菜单");
    check(strcmp(m.title, "换人") == 0, "标题 = %s", m.title ? m.title : "(null)");
    /* 开局没人濒死，但**当前出战的自己**不进列表 —— 换成自己等于白送一回合 */
    check(m.count == TEAM_SIZE - 1, "列出除出战外的全部（%d 只）", m.count);
    check(m.rows == 6, "一屏 6 行（实际 %d）", m.rows);
    check(m.scrollbar == false, "6 项以内不画滚动条");
    /* 行文字 = 种族名，行右侧 = 当前 HP。两边必须来自**同一个队伍槽**，
     * 否则会出现「名字是 A、血量是 B」的错位 —— 这正是菜单序号 != 队伍槽号
     * 那类 bug 在菜单层的样子。 */
    for (int i = 0; i < m.count; i++) {
        uint8_t slot = r.s.ui.model.switch_slot[i];
        const BattleMon *bm = &r.s.b.me.team[slot];
        const SpeciesDef *sd = species_get(bm->active_pid);
        char hp[16];
        snprintf(hp, sizeof(hp), "%u/%u", (unsigned)bm->cur_hp,
                 (unsigned)bm->hp_max);
        check(m.items[i] && sd && strcmp(m.items[i], sd->name_cn) == 0,
              "第 %d 行名字 = %s（队伍槽 %u）", i + 1,
              m.items[i] ? m.items[i] : "(null)", slot);
        check(m.notes[i] && strcmp(m.notes[i], hp) == 0,
              "第 %d 行血量 = %s", i + 1, m.notes[i] ? m.notes[i] : "(null)");
    }

    /* ---- 结算 ---- */
    rig_init(&r, AI_ROOKIE);
    start_battle(&r);
    int n = 0;
    while (ses_phase(&r.s) != SES_PHASE_RESULT && n < 20000) {
        ok_(&r);
        n++;
    }
    check(ses_phase(&r.s) == SES_PHASE_RESULT, "打到了结算");
    check(ses_layer(&r.s) == SES_LAYER_MENU, "结算是整屏菜单层");
    check(ses_menu_view(&r.s, &m), "结算要画菜单");
    check(m.big && strcmp(m.big, ses_result_text(&r.s)) == 0,
          "大字 = 结算文字（%s）", m.big ? m.big : "(null)");
    check(m.count == 1 && m.rows == 1, "结算没有可选项");
    check(m.items[0] == NULL, "结算不画列表行（big 非 NULL 时列表被忽略）");
    int alive = 0;
    for (int i = 0; i < TEAM_SIZE; i++) {
        if (r.s.b.me.team[i].cur_hp > 0) {
            alive++;
        }
    }
    char want_sub[32];
    snprintf(want_sub, sizeof(want_sub), "我方剩余 %d 只", alive);
    check(m.sub && strcmp(m.sub, want_sub) == 0, "小字 = %s（实际 %s）",
          want_sub, m.sub ? m.sub : "(null)");
    /* 配色档位跟着结果走，而不是让绘制层去看文字 */
    if (r.s.b.result == BATTLE_WIN) {
        check(m.big_style == UI_BIG_WIN, "赢了给红档");
    } else if (r.s.b.result == BATTLE_LOSE) {
        check(m.big_style == UI_BIG_LOSE, "输了给蓝档");
    } else {
        check(m.big_style == UI_BIG_DRAW, "平局给灰档");
    }
}

/*
 * 整局打完，同时把「每一条日志都塞得进对话框」这条版式不变量钉住。
 */
static void test_full_battle(void)
{
    printf("\n[整局] 一路打到结算，并检查每条日志都放得下\n");
    Rig r;
    rig_init(&r, AI_ROOKIE);
    start_battle(&r);

    int events = 0;
    int bad_pp_move = 0;
    while (ses_phase(&r.s) != SES_PHASE_RESULT && events < 20000) {
        /* 每一次出招都必须是「菜单里那一项对应的槽真的有 PP」 */
        if (ses_phase(&r.s) == SES_PHASE_INPUT &&
            ui_state_screen(&r.s.ui) == UI_SCREEN_BATTLE) {
            const BattleMon *me = battle_active_c(&r.s.b, 0);
            int c = ui_state_cursor(&r.s.ui);
            int n = r.s.ui.model.move_count;
            if (!r.s.struggle_menu && me && n > 0 && c >= 0 && c < n) {
                uint8_t slot = r.s.ui.model.move_slot[c];
                if (me->pp[slot] == 0) {
                    bad_pp_move++;
                }
            }
        }
        ok_(&r);
        events++;
    }
    check(ses_phase(&r.s) == SES_PHASE_RESULT, "整局能打到结算（%d 次按键，%s）",
          events, phase_name(ses_phase(&r.s)));
    check(events < 20000, "没有陷在某个界面里空转（上限 20000）");
    check(ses_result_text(&r.s)[0] != '\0', "有结算文字（%s）",
          ses_result_text(&r.s));
    check(r.s.b.result != BATTLE_ONGOING, "结果位有值（%u）", r.s.b.result);
    check(bad_pp_move == 0, "全程没有一次选出 0 PP 的招式（%d 次）",
          bad_pp_move);
    check(r.s.b.turn >= 2, "至少打了 2 个回合（%u）", r.s.b.turn);
    check(r.s.b.log.count > 10, "日志有 %u 条", r.s.b.log.count);

    /* 版式不变量：引擎写的每一条日志都要能在对话框的两行里放下 */
    int too_long = 0, empty_lines = 0;
    char a[96], b[96], joined[192];
    for (uint32_t i = 0; i < r.s.b.log.count; i++) {
        const char *line = ses_log_at(&r.s, i);
        if (!line) {
            continue;
        }
        int n = font_wrap2(&kFontApp16, line, UI_DLG_TEXT_W,
                           a, sizeof(a), b, sizeof(b));
        if (n > 2) {
            if (too_long < 3) {
                printf("       ! 塞不下：%s\n", line);
            }
            too_long++;
        }
        snprintf(joined, sizeof(joined), "%s%s", a, b);
        if (strcmp(joined, line) != 0) {
            empty_lines++;
        }
    }
    check(too_long == 0, "%u 条日志全部放得进两行（塞不下的 %d 条）",
          r.s.b.log.count, too_long);
    check(empty_lines == 0, "折行后拼回去都与原文一致（不一致 %d 条）",
          empty_lines);
}

int main(void)
{
    printf("会话层自检（把状态机接到引擎）\n");
    printf("==============================\n");

    test_init();
    test_intro();
    test_first_turn();
    test_menu_model();
    test_voluntary_switch();
    test_forced_switch();
    test_struggle();
    test_forfeit();
    test_result_and_replay();
    test_log_skip_and_swallow();
    test_view();
    test_team_screen();
    test_menu_view();
    test_mega();
    test_device_path();
    test_hint_width();
    test_status_hint_fit();
    test_wrap();
    test_full_battle();

    printf("\n=== 结果: %d 通过, %d 失败 ===\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
