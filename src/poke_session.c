/*
 * 会话层实现。阶段划分、设计理由与「为什么值得单独一层」见 poke_session.h。
 */
#include "poke_session.h"

#include <stdio.h>
#include <string.h>

#include "poke_species.h"
#include "poke_sprites.h"

/* ---------------------------------------------------------------- 默认队伍 */

/*
 * 队伍配置界面（能改性格/努力值/道具的选择器，路线图第 5 项）还没做，
 * 先用一份手写死的队伍把全流程跑通。缺的只是「选中之后改写属性」——
 * 386 只的图鉴**列表**已经能画能选了。
 *
 * 刻意**不带特性与道具**：这一层的自检要能逐回合重放，特性（威吓、天气）
 * 与道具的行为已经在 test_battle / test_ability / test_item 里断言过，
 * 不在这里重复引入变量。等队伍配置做好，这份表就退休。
 *
 * 招式 id 是 Veekun 的 move id（gen3 范围），中文名见注释。
 */
#define SES_DEFAULT_LEVEL 50

typedef struct {
    uint16_t pid;          /* pokemon id */
    uint16_t moves[4];     /* Veekun move id，0 = 空槽 */
} SesMonSpec;

static const SesMonSpec kTeamMe[TEAM_SIZE] = {
    {   6, {  53, 337,  17,  14 } },  /* 喷火龙：喷射火焰 / 龙爪 / 翅膀攻击 / 剑舞 */
    {   9, {  57,  58,  34, 105 } },  /* 水箭龟：冲浪 / 冰冻光束 / 泰山压顶 / 自我再生 */
    {   3, { 188, 202,  75,  73 } },  /* 妙蛙花：污泥炸弹 / 终极吸取 / 飞叶快刀 / 寄生种子 */
    {  25, {  85,  86,  98,  34 } },  /* 皮卡丘：十万伏特 / 电磁波 / 电光一闪 / 泰山压顶 */
    {  65, {  94, 247, 105, 347 } },  /* 胡地：  精神强念 / 暗影球 / 自我再生 / 冥想 */
    { 130, {  56,  89, 242, 157 } },  /* 暴鲤龙：水炮 / 地震 / 咬碎 / 岩崩 */
};

static const SesMonSpec kTeamFoe[TEAM_SIZE] = {
    { 376, { 309,  89, 157, 332 } },  /* 巨金怪：  彗星拳 / 地震 / 岩崩 / 燕返 */
    { 254, {  75, 202,  14, 280 } },  /* 蜥蜴王：  飞叶快刀 / 终极吸取 / 剑舞 / 劈瓦 */
    { 282, {  94, 247, 347, 105 } },  /* 沙奈朵：  精神强念 / 暗影球 / 冥想 / 自我再生 */
    { 359, { 242, 247, 332,  14 } },  /* 阿勃梭鲁：咬碎 / 暗影球 / 燕返 / 剑舞 */
    { 306, {  89, 157, 309,  34 } },  /* 波士可多拉：地震 / 岩崩 / 彗星拳 / 泰山压顶 */
    {   6, {  53, 337, 126,  17 } },  /* 喷火龙：  喷射火焰 / 龙爪 / 大字爆炎 / 翅膀攻击 */
};

static void load_side(Battle *b, int side_is_foe, const SesMonSpec *spec)
{
    for (int i = 0; i < TEAM_SIZE; i++) {
        BattleMon m;
        mon_init(&m, spec[i].pid, SES_DEFAULT_LEVEL, 0, ABIL_NONE, 0,
                 spec[i].moves);
        battle_set_mon(b, side_is_foe, (uint8_t)i, &m);
    }
}

/* ---------------------------------------------------------------- 小工具 */

static const char *mon_cn(const BattleMon *m)
{
    const SpeciesDef *sd = species_get(m ? m->active_pid : 0);
    return (sd && sd->name_cn) ? sd->name_cn : "???";
}

/*
 * 立绘。**Mega 形态的图存在 SPR_MEGA 那一档里**，用 SPR_FRONT 是查不到的
 * （精灵表按 (pid, kind) 精确匹配）—— 直接拿 active_pid 去查 SPR_FRONT，
 * Mega 之后就会「血条框还在、立绘凭空消失」。
 */
static const SpriteEntry *mon_sprite(const BattleMon *m)
{
    if (!m) {
        return NULL;
    }
    const SpeciesDef *sd = species_get(m->active_pid);
    uint8_t kind = (sd && sd->is_mega) ? SPR_MEGA : SPR_FRONT;
    return sprite_get(m->active_pid, kind);
}

/*
 * 日志的**绝对序号** → 文字。
 *
 * 不能直接用 log_get()：那个下标是「环形缓冲里还剩的第 i 条」，日志一旦
 * 超过 MAX_LOG 开始覆盖，同一个下标指的内容就变了。播放游标必须在覆盖
 * 发生前后都指着同一条日志，所以自己把绝对序号换算成相对下标。
 * 已经被覆盖掉的（abs < start）返回 NULL —— 那种情况只会出现在「快进」
 * 一次跨过 MAX_LOG 条日志时，正常对局不会发生。
 */
const char *ses_log_at(const Session *s, uint32_t abs_index)
{
    if (!s) {
        return NULL;
    }
    const BattleLog *l = &s->b.log;
    if (l->count == 0 || abs_index >= (uint32_t)l->count) {
        return NULL;
    }
    uint32_t start = (l->count > MAX_LOG) ? (uint32_t)(l->count - MAX_LOG) : 0;
    if (abs_index < start) {
        return NULL;
    }
    return log_get(l, (uint16_t)(abs_index - start));
}

uint32_t ses_log_index(const Session *s)
{
    if (!s) {
        return 0;
    }
    /* 播放中显示「下一条要放的」；播完之后（等输入 / 结算 / 主菜单）
     * 停在最后放过的那条上，对话框不会突然空掉。 */
    if (s->phase == SES_PHASE_LOG) {
        return s->log_next;
    }
    return s->log_next > 0 ? s->log_next - 1 : 0;
}

/* ---------------------------------------------------------------- 界面模型 */

/*
 * 把引擎状态翻成状态机要的「菜单项数 + 序号到数据下标的映射」。
 *
 * 每回合都要重算：出招耗 PP（菜单里的项会少一个）、宝可梦倒下（换人列表
 * 会少一个）。不重算的话光标会停在一个已经不能选的项上。
 */
static void refresh_model(Session *s)
{
    UiModel m;
    memset(&m, 0, sizeof(m));

    const BattleMon *me = battle_active_c(&s->b, 0);

    /* 招式菜单只列「有招式且还有 PP」的槽，所以菜单序号 != 招式槽号 */
    int n = 0;
    s->struggle_menu = false;
    if (me) {
        for (int i = 0; i < MOVES_PER_MON; i++) {
            if (me->move[i] && me->pp[i] > 0) {
                m.move_slot[n++] = (uint8_t)i;
            }
        }
        if (n == 0) {
            /*
             * PP 全空（或压根没装招式）→ 菜单只留一项「挣扎」。
             *
             * **这一项是防卡死用的**：引擎在「所有招式 PP 都用光」时会把
             * 任何出招请求当成挣扎（见 do_move），但界面如果给出一个空菜单，
             * 玩家连请求都发不出去，回合永远推不动。槽号随便填一个真的有
             * 招式的槽 —— 引擎在挣扎分支里不看它是哪一个。
             */
            int slot = 0;
            for (int i = 0; i < MOVES_PER_MON; i++) {
                if (me->move[i]) {
                    slot = i;
                    break;
                }
            }
            m.move_slot[0] = (uint8_t)slot;
            n = 1;
            s->struggle_menu = true;
        }
    }
    m.move_count = (uint8_t)n;

    /* 换人菜单只列还活着的、且不是当前出战的 —— 换成自己等于白送一回合 */
    int k = 0;
    for (int i = 0; i < TEAM_SIZE; i++) {
        const BattleMon *t = &s->b.me.team[i];
        if (i != (int)s->b.me.active && t->alive && t->cur_hp > 0) {
            m.switch_slot[k++] = (uint8_t)i;
        }
    }
    m.switch_count = (uint8_t)k;
    m.species_count = UI_TEAM_COUNT;

    ui_state_set_model(&s->ui, &m);
}

/* ---------------------------------------------------------------- 回合流程 */

/* 这几条互相调用（begin_log ⇄ finish_log ⇄ 换人 → begin_log），
 * 先声明再定义，省得为了顺序把函数摆得七零八落。 */
static void begin_log(Session *s);
static void finish_log(Session *s);
static void enter_input(Session *s);
static void enter_force_switch(Session *s);
static void ai_forced_switch(Session *s, int side_is_foe);
static void to_result(Session *s);

/*
 * 日志播完了，决定下一步。分支本身可能又产生日志（AI 换人也会写日志），
 * 所以写成循环而不是递归 —— 递归在这里没有表达力上的好处，反而要担心
 * 深度。四圈足够：结果 / 强制换人 / 对手换人 / 正常输入，一条路只走一次。
 */
static void finish_log(Session *s)
{
    for (int guard = 0; guard < 4; guard++) {
        if (s->b.result != BATTLE_ONGOING) {
            to_result(s);
            return;
        }
        if (s->b.need_switch_me) {
            enter_force_switch(s);
            return;
        }
        if (s->b.need_switch_foe) {
            ai_forced_switch(s, 1);
            if (s->b.log.count > s->log_end) {
                begin_log(s);   /* 换人也有日志，接着播 */
                return;
            }
            continue;
        }
        enter_input(s);
        return;
    }
    /* 兜底：绝不停在「日志播完了但没人收键」的中间态 */
    enter_input(s);
}

/*
 * 开始播放「引擎新写的那几条日志」。
 *
 * log_next 是绝对序号，指向第一条还没显示的；log_end 是本次播放的终点。
 * 两者分开记是为了「一场对局播多次」—— 每回合都有自己的终点。
 */
static void begin_log(Session *s)
{
    s->log_end = s->b.log.count;
    if (s->log_next >= s->log_end) {
        finish_log(s);   /* 没有新日志：直接决定下一步（护栏触发时就是这样） */
        return;
    }
    s->phase = SES_PHASE_LOG;
}

static void log_step(Session *s, uint32_t n)
{
    if (n == 0) {
        finish_log(s);
        return;
    }
    s->log_next += n;
    if (s->log_next >= s->log_end) {
        s->log_next = s->log_end;
        finish_log(s);
    }
    s->log_advances++;
}

static void enter_input(Session *s)
{
    refresh_model(s);
    if (ui_state_screen(&s->ui) != UI_SCREEN_BATTLE) {
        /* 正常情况下已经停在战斗主界面（出招/换人都在那儿收尾）；
         * 这一句是防御性的，免得哪天多出个界面把键吞掉。 */
        ui_state_begin_battle(&s->ui);
    }
    s->phase = SES_PHASE_INPUT;
}

static void enter_force_switch(Session *s)
{
    refresh_model(s);
    uint8_t slots[UI_TEAM_SLOTS];
    int n = s->ui.model.switch_count;
    for (int i = 0; i < n; i++) {
        slots[i] = s->ui.model.switch_slot[i];
    }
    if (n == 0) {
        /* 一只可换的都没有，却在等换人 —— 只可能是引擎那边已经判完了。
         * 交给 finish_log 的下一圈去处理，别把用户锁死在一个空菜单上。 */
        enter_input(s);
        return;
    }
    s->force_pending = true;
    ui_state_force_switch(&s->ui, slots, n);
    s->phase = SES_PHASE_INPUT;
}

static void to_result(Session *s)
{
    s->force_pending = false;
    ui_state_show_result(&s->ui, (int)s->b.result);
    s->phase = SES_PHASE_RESULT;
}

/* 对手决定这一回合出什么。ai_choose 内部会静音日志并保存/恢复随机流，
 * 所以搜索不会消耗真实对局的熵（见 poke_ai.h）。 */
static void ai_foe_act(Session *s)
{
    AIAction a;
    ai_choose(&s->b, 1, &s->ai, &a);
    ai_apply(&s->b, 1, &a);
}

/*
 * 对手的出战宝可梦濒死，AI 挑一只换上去。
 *
 * **必须直接 battle_switch，不能走 battle_step**：引擎在出战濒死时会拒绝
 * 推进回合（防呆护栏，否则双方都 continue、整回合静默空转），所以
 * 「让 battle_step 去执行换人」这条路根本走不通。
 */
static void ai_forced_switch(Session *s, int side_is_foe)
{
    AIAction a;
    ai_choose(&s->b, side_is_foe, &s->ai, &a);
    if (!(a.is_switch && battle_switch(&s->b, side_is_foe, a.slot) == 0)) {
        /* 兜底：AI 没给换人候选时自己找第一只活着的 */
        const BattleSide *side = side_is_foe ? &s->b.foe : &s->b.me;
        for (int i = 0; i < TEAM_SIZE; i++) {
            if (i != (int)side->active && side->team[i].alive &&
                side->team[i].cur_hp > 0) {
                battle_switch(&s->b, side_is_foe, (uint8_t)i);
                break;
            }
        }
    }
    if (side_is_foe) {
        s->b.need_switch_foe = 0;
    } else {
        s->b.need_switch_me = 0;
    }
}

/* 推进一个回合：对手先行动，再结算。返回值不用看 —— 需要换人这件事
 * 已经写在 b.need_switch_me / need_switch_foe 上（引擎两个地方都设了）。 */
static void run_turn(Session *s)
{
    ai_foe_act(s);
    battle_step(&s->b);
    begin_log(s);
}

/* 我方强制换人（出战那只已倒下）。同样不能走 battle_step。 */
static void do_forced_switch(Session *s, uint8_t slot)
{
    s->force_pending = false;
    if (battle_switch(&s->b, 0, slot) != 0) {
        /* 目标不合法。换人菜单只列活着的，正常到不了这里；真到了就让他重选。 */
        enter_force_switch(s);
        return;
    }
    s->b.need_switch_me = 0;
    begin_log(s);
}

/* ---------------------------------------------------------------- 动作 */

static void apply(Session *s, const UiResult *r)
{
    switch (r->act) {
    case UI_ACT_START_BATTLE:
        ses_begin(s);
        return;

    case UI_ACT_CHOOSE_MOVE:
        s->b.my_action = (uint8_t)r->arg;
        s->b.my_switch = 0;
        run_turn(s);
        return;

    case UI_ACT_SWITCH_MON:
        if (s->force_pending) {
            do_forced_switch(s, (uint8_t)r->arg);
        } else {
            s->b.my_action = 0xFF;          /* 换人是「放弃出招」，不是招式 0 */
            s->b.my_switch = (uint8_t)r->arg;
            run_turn(s);
        }
        return;

    case UI_ACT_FORFEIT:
        /* 认输不是招式结算，是弃权：没有回合可推进，直接把结果置为负。 */
        s->b.result = BATTLE_LOSE;
        to_result(s);
        return;

    case UI_ACT_PICK_SPECIES:
    case UI_ACT_NONE:
    default:
        /* 队伍编辑还没做，选完图鉴不做任何事 */
        return;
    }
}

/* ---------------------------------------------------------------- 对外 */

void ses_load_default_teams(Session *s)
{
    if (!s) {
        return;
    }
    battle_init(&s->b);
    s->b.me.name = "我方";
    s->b.foe.name = "对手";
    load_side(&s->b, 0, kTeamMe);
    load_side(&s->b, 1, kTeamFoe);
}

void ses_init(Session *s, uint8_t ai_level)
{
    if (!s) {
        return;
    }
    *s = (Session){ 0 };
    s->swallow_key = -1;
    ai_config_default(&s->ai, ai_level);
    key_init(&s->keys);
    ui_state_init(&s->ui);
    ses_load_default_teams(s);
    refresh_model(s);
    s->phase = SES_PHASE_MAIN;
}

void ses_set_fonts(Session *s, const UiFonts *fonts)
{
    if (s) {
        s->fonts = fonts;
    }
}

void ses_begin(Session *s)
{
    if (!s) {
        return;
    }
    /*
     * 从空日志开始。battle_start 是**追加**开场日志的，重开一局时不先清掉
     * 就会连着上一局的记录一起播（上一轮的游标又是从头数的）。
     */
    log_init(&s->b.log);
    s->b.result = BATTLE_ONGOING;
    s->b.need_switch_me = 0;
    s->b.need_switch_foe = 0;
    battle_log_enable(true);   /* AI 搜索会临时静音，确保开回来 */
    battle_start(&s->b);

    s->log_next = 0;
    s->log_end = 0;
    s->swallow_key = -1;
    s->force_pending = false;
    s->log_advances = 0;
    s->log_skips = 0;

    ui_state_begin_battle(&s->ui);
    refresh_model(s);
    begin_log(s);   /* 开场那三条也要逐条播 */
}

void ses_to_main(Session *s)
{
    if (!s) {
        return;
    }
    /*
     * 回主菜单顺带把队伍复位。队伍配置界面还没做，所以「再来一局」的语义
     * 就是「用默认队伍重新开始」—— 不复位的话第二局会带着上一局的残血和
     * 空 PP 开打。等队伍配置做好，这里改成保留玩家编辑过的队伍。
     */
    ses_load_default_teams(s);
    ui_state_init(&s->ui);
    refresh_model(s);
    s->force_pending = false;
    s->log_next = 0;
    s->log_end = 0;
    s->phase = SES_PHASE_MAIN;
}

int ses_player_mega(Session *s, uint16_t mega_pid)
{
    if (!s || s->phase != SES_PHASE_INPUT) {
        return -1;
    }
    if (ui_state_screen(&s->ui) != UI_SCREEN_BATTLE) {
        return -1;   /* 弹层/换人界面里不算「出招前」 */
    }
    int rc = battle_mega(&s->b, 0, mega_pid);
    if (rc == 0) {
        /* 「超级进化了」那句也是日志，要接着播 —— 不播的话它会被压在
         * 对话框里直到下一次换行，用户根本看不见。 */
        begin_log(s);
    }
    return rc;
}

void ses_feed(Session *s, const KeyMsg *msg, uint32_t now_ms)
{
    if (!s || !msg) {
        return;
    }

    /*
     * 本层自己消化掉一次长按之后，紧随其后的 CLICK 要吞掉 —— 官方按键
     * 组件某些配置会把 LONG 与 CLICK 都报出来，不挡就会「长按跳过日志」
     * 顺手把下面那一项也确定了。
     *
     * 只在**下一次按下**时作废这条规则，不在抬起时作废：LONG 与 CLICK 的
     * 先后顺序上游没有保证（实测两种都有），抬起就作废会漏掉
     * 「LONG → 抬起 → CLICK」那种顺序。一次按压最多带一个 CLICK，
     * 所以等下一次按下再来认领是安全的。
     */
    if (s->swallow_key >= 0) {
        if (msg->ev == KEY_EV_CLICK && (int)msg->key == s->swallow_key) {
            s->swallow_key = -1;
            return;
        }
        if (msg->ev == KEY_EV_PRESS) {
            s->swallow_key = -1;
        }
    }

    switch (s->phase) {
    case SES_PHASE_LOG:
        /* 播放中按键不进状态机：这两个界面没有导航，键的意义由本层解释 */
        if (msg->key != KEY_OK) {
            return;
        }
        if (msg->ev == KEY_EV_CLICK) {
            log_step(s, 1);
        } else if (msg->ev == KEY_EV_LONG) {
            s->swallow_key = (int8_t)KEY_OK;
            s->log_skips++;
            log_step(s, s->log_end - s->log_next);
        }
        return;

    case SES_PHASE_RESULT:
        if (msg->key != KEY_OK) {
            return;
        }
        if (msg->ev == KEY_EV_LONG) {
            s->swallow_key = (int8_t)KEY_OK;
            ses_to_main(s);
        } else if (msg->ev == KEY_EV_CLICK) {
            ses_to_main(s);
        }
        return;

    case SES_PHASE_MAIN:
    case SES_PHASE_INPUT:
    default:
        break;
    }

    UiResult r;
    if (ui_state_feed(&s->ui, msg, now_ms, &r)) {
        apply(s, &r);
    }
}

void ses_tick(Session *s, uint32_t now_ms)
{
    if (!s) {
        return;
    }
    if (s->phase != SES_PHASE_MAIN && s->phase != SES_PHASE_INPUT) {
        return;   /* 播放日志 / 结算时不接受「按住连发」 */
    }
    ui_state_tick(&s->ui, now_ms, NULL);
}

void ses_poll(Session *s, uint32_t now_ms, int mv)
{
    if (!s) {
        return;
    }
    key_poll(&s->keys, now_ms, mv);
    KeyMsg m;
    while (key_next(&s->keys, &m)) {
        ses_feed(s, &m, now_ms);
    }
    ses_tick(s, now_ms);
}

/* ---------------------------------------------------------------- 查询 */

SesPhase ses_phase(const Session *s)
{
    return s ? s->phase : SES_PHASE_MAIN;
}

const Battle *ses_battle_c(const Session *s)
{
    return s ? &s->b : NULL;
}

Battle *ses_battle(Session *s)
{
    return s ? &s->b : NULL;
}

const UiState *ses_ui_c(const Session *s)
{
    return s ? &s->ui : NULL;
}

const char *ses_row_text(const Session *s, int index)
{
    if (!s || index < 0) {
        return NULL;
    }
    UiScreen sc = ui_state_screen(&s->ui);
    if (index >= ui_state_count(&s->ui)) {
        return NULL;
    }
    switch (sc) {
    case UI_SCREEN_TEAM: {
        /* 图鉴列表：第 i 项就是图鉴序号 i+1（列表没做筛选/排序） */
        const SpeciesDef *sd = species_get((uint16_t)(index + 1));
        return (sd && sd->name_cn) ? sd->name_cn : NULL;
    }
    case UI_SCREEN_SWITCH: {
        if (index >= s->ui.model.switch_count) {
            return NULL;
        }
        uint8_t slot = s->ui.model.switch_slot[index];
        if (slot >= TEAM_SIZE) {
            return NULL;
        }
        return mon_cn(&s->b.me.team[slot]);
    }
    default:
        /* 主菜单 / 弹层 / 认输确认：文字是写死的，由状态机自己给 */
        return ui_item_label(sc, index);
    }
}

const char *ses_result_text(const Session *s)
{
    if (!s) {
        return "";
    }
    switch (s->b.result) {
    case BATTLE_WIN:  return "胜利";
    case BATTLE_LOSE: return "失败";
    case BATTLE_DRAW: return "平局";
    default:          return "";
    }
}

void ses_view(Session *s, BattleScreen *out)
{
    if (!s || !out) {
        return;
    }
    memset(out, 0, sizeof(*out));

    const BattleMon *me  = battle_active_c(&s->b, 0);
    const BattleMon *foe = battle_active_c(&s->b, 1);

    /* ---- 两个血条框 ---- */
    /* 精灵图与名字都取 active_pid：Mega 之后形态变了，显示的也该是新形态 */
    if (foe) {
        out->foe_sprite    = mon_sprite(foe);
        out->foe_name      = mon_cn(foe);
        out->foe_hp_permil = foe->hp_max
                             ? (int)((uint32_t)foe->cur_hp * 1000 / foe->hp_max)
                             : 0;
        snprintf(s->lv_text[1], sizeof(s->lv_text[1]), "Lv%u",
                 (unsigned)foe->base.level);
        out->foe_lv = s->lv_text[1];
    }
    if (me) {
        out->me_sprite    = mon_sprite(me);
        out->me_name      = mon_cn(me);
        out->me_hp_permil = me->hp_max
                            ? (int)((uint32_t)me->cur_hp * 1000 / me->hp_max)
                            : 0;
        snprintf(s->lv_text[0], sizeof(s->lv_text[0]), "Lv%u",
                 (unsigned)me->base.level);
        snprintf(s->hp_text[0], sizeof(s->hp_text[0]), "%u/%u",
                 (unsigned)me->cur_hp, (unsigned)me->hp_max);
        /* 这两个指针必须挂上：文字写进了 s->lv_text/hp_text，但绘制读的是
         * out 里的指针。漏掉赋值不会崩，只是血条框上**等级和 HP 数字凭空
         * 消失** —— 正是「画面上少了一块而测试全绿」那一类。 */
        out->me_lv      = s->lv_text[0];
        out->me_hp_text = s->hp_text[0];

        /* ---- 招式菜单：菜单项 → 招式槽 → 名字与 PP ---- */
        int n = s->ui.model.move_count;
        if (s->struggle_menu) {
            /* PP 全空时菜单只有一项，写死成「挣扎」（与引擎实际会使出的招式
             * 一致）—— 不能照抄槽里的招式名，那会让玩家以为还能用那招。 */
            out->moves[0] = "挣扎";
        } else {
            for (int i = 0; i < MOVES_PER_MON; i++) {
                if (i >= n) {
                    break;
                }
                uint8_t slot = s->ui.model.move_slot[i];
                if (slot >= MOVES_PER_MON) {
                    continue;
                }
                const MoveDef *mv = mv_get(me->move[slot]);
                out->moves[i] = (mv && mv->name_cn) ? mv->name_cn : NULL;
                snprintf(s->pp_text[i], sizeof(s->pp_text[i]), "%u",
                         (unsigned)me->pp[slot]);
                out->move_pp[i] = s->pp_text[i];
            }
        }
    }

    /* ---- 对话框：当前这条日志（折成两行）---- */
    const char *msg = ses_log_at(s, ses_log_index(s));
    s->log_text[0][0] = '\0';
    s->log_text[1][0] = '\0';
    if (msg) {
        if (s->fonts && s->fonts->dialog) {
            font_wrap2(s->fonts->dialog, msg, UI_DLG_TEXT_W,
                       s->log_text[0], SES_LOG_LINE_MAX,
                       s->log_text[1], SES_LOG_LINE_MAX);
        } else {
            /* 没给字体就当它一行放得下 —— 自检里只关心文字内容 */
            snprintf(s->log_text[0], SES_LOG_LINE_MAX, "%s", msg);
        }
    }
    out->log[0] = s->log_text[0][0] ? s->log_text[0] : NULL;
    out->log[1] = s->log_text[1][0] ? s->log_text[1] : NULL;

    /* ---- 菜单高亮 ---- */
    out->selected = (s->phase == SES_PHASE_INPUT &&
                     ui_state_screen(&s->ui) == UI_SCREEN_BATTLE)
                    ? ui_state_cursor(&s->ui)
                    : -1;

    /* ---- 状态行 ---- */
    s->status_left[0] = '\0';
    switch (s->phase) {
    case SES_PHASE_LOG:
        /* 播放日志时左侧报回合数：日志刷得太快，用户需要知道打到第几回合了 */
        snprintf(s->status_left, sizeof(s->status_left), "第 %u 回合",
                 (unsigned)s->b.turn);
        snprintf(s->status_right, sizeof(s->status_right), "%s", "确定   长按跳过");
        break;

    case SES_PHASE_INPUT: {
        UiScreen sc = ui_state_screen(&s->ui);
        if (s->struggle_menu) {
            /* PP 全空：左侧报「挣扎」。报「PP 0/15」会让人以为还能选那招。 */
            snprintf(s->status_left, sizeof(s->status_left), "%s", "挣扎");
        } else if (sc == UI_SCREEN_BATTLE && me) {
            int n = s->ui.model.move_count;
            int c = ui_state_cursor(&s->ui);
            if (n > 0 && c >= 0 && c < n) {
                uint8_t slot = s->ui.model.move_slot[c];
                snprintf(s->status_left, sizeof(s->status_left), "PP %u/%u",
                         (unsigned)me->pp[slot], (unsigned)me->pp_max[slot]);
            }
        }
        snprintf(s->status_right, sizeof(s->status_right), "%s",
                 ui_screen_hint(sc));
        break;
    }

    case SES_PHASE_RESULT:
        snprintf(s->status_right, sizeof(s->status_right), "%s", "确定继续");
        break;

    case SES_PHASE_MAIN:
    default:
        snprintf(s->status_right, sizeof(s->status_right), "%s",
                 ui_screen_hint(ui_state_screen(&s->ui)));
        break;
    }
    out->status_left  = s->status_left[0] ? s->status_left : NULL;
    out->status_right = s->status_right[0] ? s->status_right : NULL;
}

/* ================================================================ 分层 */

SesLayer ses_layer(const Session *s)
{
    if (!s) {
        return SES_LAYER_MENU;
    }
    switch (s->phase) {
    case SES_PHASE_MAIN:
    case SES_PHASE_RESULT:
        /* 主菜单时战斗还没开始（battle_start 还没调），这时画战斗界面会
         * 画出两个空血条框和一片没有立绘的场景 —— 所以是「只画菜单」，
         * 不是「战斗 + 菜单」。 */
        return SES_LAYER_MENU;

    case SES_PHASE_LOG:
        /* 日志播放中：屏幕上是战斗界面，对话框里一条条放战况。
         * **不是菜单层** —— 这里按键由会话自己解释，状态机的界面停在哪
         * 不重要（可能是招式菜单，但那时不该接受导航）。 */
        return SES_LAYER_BATTLE;

    case SES_PHASE_INPUT:
    default: {
        UiScreen sc = ui_state_screen(&s->ui);
        if (sc == UI_SCREEN_ACTION || sc == UI_SCREEN_CONFIRM) {
            /* 弹层 / 认输确认：盖在战斗界面上，对话框里的战况日志要继续看得见 */
            return SES_LAYER_BATTLE_MENU;
        }
        if (sc == UI_SCREEN_SWITCH) {
            /* 换人是**整屏**队伍列表，不是弹层：一行要放名字 + 当前 HP，
             * 招式框那 232x64 放不下 6 只。 */
            return SES_LAYER_MENU;
        }
        return SES_LAYER_BATTLE;   /* 招式菜单本身就是战斗界面的一部分 */
    }
    }
}

/* ================================================================ 菜单数据 */

bool ses_menu_view(Session *s, MenuScreen *out)
{
    if (!s || !out) {
        return false;
    }
    memset(out, 0, sizeof(*out));

    SesLayer layer = ses_layer(s);
    if (layer == SES_LAYER_BATTLE) {
        return false;                 /* 这一屏没有菜单要画 */
    }

    UiScreen sc = ui_state_screen(&s->ui);

    out->title    = ui_screen_title(sc);
    out->count    = ui_state_count(&s->ui);
    out->top      = ui_state_top(&s->ui);
    out->selected = ui_state_cursor(&s->ui);
    out->status_right = ui_screen_hint(sc);

    /* 行数由**状态机**给，不在这里另写一份：一屏几行是「列表怎么排」的
     * 决定，状态机按它算滚动窗口，绘制按它摆行 —— 两边对不上就会出现
     * 「光标移到了看不见的第 9 项」。 */
    out->rows = ui_screen_rows(sc);
    if (out->rows > UI_MENU_ROWS_MAX) {
        out->rows = UI_MENU_ROWS_MAX;   /* 版式只留了这么多行的高度 */
    }
    if (out->rows < 1) {
        out->rows = 1;
    }
    out->scrollbar = (out->count > out->rows);

    /* ---- 结算：没有列表，一颗大字 ---- */
    if (sc == UI_SCREEN_RESULT) {
        out->big = ses_result_text(s);
        switch (s->b.result) {
        case BATTLE_WIN:  out->big_style = UI_BIG_WIN;  break;
        case BATTLE_LOSE: out->big_style = UI_BIG_LOSE; break;
        default:          out->big_style = UI_BIG_DRAW; break;
        }
        int alive = 0;
        for (int i = 0; i < TEAM_SIZE; i++) {
            if (s->b.me.team[i].cur_hp > 0) {
                alive++;
            }
        }
        snprintf(s->menu_sub, sizeof(s->menu_sub), "我方剩余 %d 只", alive);
        out->sub = s->menu_sub;
        return true;
    }

    /* ---- 状态行左侧 ----
     * 只有图鉴列表需要：386 项、一屏 8 行，不报序号的话根本不知道看的是第几只。 */
    s->menu_left[0] = '\0';
    if (sc == UI_SCREEN_TEAM && out->count > 0) {
        snprintf(s->menu_left, sizeof(s->menu_left), "%d/%d",
                 out->selected + 1, out->count);
        out->status_left = s->menu_left;
    }

    /* ---- 行文字：只填窗口内的那几行 ----
     * 按 count 填会白写 386 次 snprintf（队伍配置），而且 items[] 只有
     * UI_MENU_ROWS_MAX 项，越界写。 */
    int shown = out->rows;
    if (out->top >= out->count) {
        shown = 0;
    } else if (shown > out->count - out->top) {
        shown = out->count - out->top;
    }
    for (int i = 0; i < shown; i++) {
        out->items[i] = ses_row_text(s, out->top + i);
    }

    /* ---- 换人列表：每行右侧挂当前 HP ----
     * 换人时唯一要比较的就是「这只还剩多少血」，挂在名字右边比另开一行好，
     * 而且这里的顺序就是**队伍槽顺序**（模型给的是还活着的那几只）。 */
    if (sc == UI_SCREEN_SWITCH) {
        for (int i = 0; i < shown; i++) {
            uint8_t slot = s->ui.model.switch_slot[out->top + i];
            if (slot >= TEAM_SIZE) {
                continue;
            }
            const BattleMon *bm = &s->b.me.team[slot];
            snprintf(s->menu_note[i], sizeof(s->menu_note[i]), "%u/%u",
                     (unsigned)bm->cur_hp, (unsigned)bm->hp_max);
            out->notes[i] = s->menu_note[i];
        }
    }

    return true;
}
