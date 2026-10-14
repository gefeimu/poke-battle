/*
 * 会话层实现。阶段划分、设计理由与「为什么值得单独一层」见 poke_session.h。
 */
#include "poke_session.h"

#include <stdio.h>
#include <string.h>

#include "poke_species.h"
#include "poke_sprites.h"
#include "poke_icons.h"

/* ---------------------------------------------------------------- 预设队伍 */

/*
 * 一只预设宝可梦的完整配置。
 *
 * 与早期「只写 pid + 招式」的版本相比，这里补上性格/努力值/特性/道具 ——
 * 队伍配置界面（能逐只改属性的选择器，路线图第 5 项）还没做，先用五支
 * 可玩的主流预设队把全流程跑通。
 *
 * ev 的下标与 mon_set_ev / mon_set_iv 一致：**1=hp 2=atk 3=def 4=spa
 * 5=spd 6=spe** —— 但数组按 0 基存，装载时 +1 传给那两个函数（写错下标
 * 会静默写偏，所以校验器专门盯这一项）。
 */
#define SES_DEFAULT_LEVEL 50
#define SES_DEFAULT_IV    31   /* 个体值默认全 31（competitive 默认） */

typedef struct {
    uint16_t pid;          /* pokemon id */
    uint8_t  nature;       /* NatureDef 下标 0..25 */
    uint8_t  ability;      /* AbilityKind */
    uint16_t item;         /* ItemDef.id，0 = 无 */
    uint8_t  ev[6];        /* [hp, atk, def, spa, spd, spe] */
    uint16_t moves[4];     /* Veekun move id，0 = 空槽 */
} SesMonSpec;

/*
 * 五支主流预设队（三代图鉴 1..386 + 第六代规则）。
 *
 * 招式 id 是 Veekun 的 move id（gen3 范围），中文名见行末注释。
 * 手写表最容易出笔误，所以 test_session 里有一份**预设队校验器**兜底：
 * species/ability/item/nature/EV/招式合法性 + 4 招互不重复，全部机器核验。
 *
 * 注意招式只保证「id 存在且不重复」——**不校验 learnset**：本项目的
 * gen/poke_learnsets.c 只含升级招式（无 TM/教招），极稀疏（如宝石海星
 * 只有高速旋转/奇异之光），按 learnset 卡会把主流配招全否掉。第六代规则
 * 下这些招式对相应宝可梦都是合法的课堂配置。
 */
static const SesMonSpec kPreset[UI_PRESET_COUNT][TEAM_SIZE] = {
    /* ---- 队 1：晴天队 ---- */
    {
        {  38, 11, 70, 226, {252,   0,   0, 252,   0,   4}, {241,  53,  76, 261} }, /* 九尾：  大晴天 / 喷射火焰 / 日光束 / 鬼火 */
        {   3, 16, 34, 216, {  4,   0,   0, 252,   0, 252}, { 76, 188,  79,  73} }, /* 妙蛙花：日光束 / 污泥炸弹 / 催眠粉 / 寄生种子 */
        { 257,  4,  3, 218, {  4, 252,   0,   0,   0, 252}, { 14, 327, 299, 157} }, /* 火焰鸡：剑舞 / 冲天拳 / 火焰踢 / 岩崩 */
        {  59,  4, 22, 226, {  4, 252,   0,   0,   0, 252}, {126, 245, 242,  46} }, /* 风速狗：大字爆炎 / 神速 / 咬碎 / 吼叫 */
        { 103, 16, 34, 211, {252,   0,   0, 252,   4,   0}, { 94,  76,  79, 153} }, /* 椰蛋树：精神强念 / 日光束 / 催眠粉 / 大爆炸 */
        { 136,  4, 18, 226, {  4, 252,   0,   0,   0, 252}, { 53,  38, 231,  44} }, /* 火伊布：喷射火焰 / 舍身冲撞 / 铁尾 / 咬住 */
    },
    /* ---- 队 2：雨天队 ---- */
    {
        { 186,  6,  2, 211, {252,   0, 252,   0,   4,   0}, {240,  57,  58,  95} }, /* 蚊香蛙皇：求雨 / 冲浪 / 冰冻光束 / 催眠术 */
        { 230, 16, 33, 227, {  4,   0,   0, 252,   0, 252}, { 56, 337,  58, 240} }, /* 刺龙王：  水炮 / 龙爪 / 冰冻光束 / 求雨 */
        { 272, 11, 33, 211, {  4,   0,   0, 252,   0, 252}, { 57, 202,  58,  73} }, /* 乐天河童：冲浪 / 终极吸取 / 冰冻光束 / 寄生种子 */
        { 141,  4, 33, 215, {  4, 252,   0,   0,   0, 252}, {127, 157,  89,  14} }, /* 镰刀盔：  攀瀑 / 岩崩 / 地震 / 剑舞 */
        { 139, 16, 33, 220, {  4,   0,   0, 252,   0, 252}, { 57,  58, 246, 157} }, /* 多刺菊石兽：冲浪 / 冰冻光束 / 原始之力 / 岩崩 */
        { 171, 16, 10, 219, {252,   0,   0, 252,   4,   0}, { 85,  57,  58,  86} }, /* 电灯怪：  十万伏特 / 冲浪 / 冰冻光束 / 电磁波 */
    },
    /* ---- 队 3：威吓物攻队 ---- */
    {
        { 130,  4, 22, 211, {  4, 252,   0,   0,   0, 252}, {349,  89, 157,  38} }, /* 暴鲤龙：  龙之舞 / 地震 / 岩崩 / 舍身冲撞 */
        { 373, 15, 22, 227, {  0, 252,   0, 252,   0,   4}, {349, 337, 157, 126} }, /* 暴飞龙：  龙之舞 / 龙爪 / 岩崩 / 大字爆炎 */
        { 217,  4, 62, 211, {  4, 252,   0,   0,   0, 252}, { 14,  34,  89, 157} }, /* 圈圈熊：  剑舞 / 泰山压顶 / 地震 / 岩崩 */
        { 212,  4,101, 209, {252, 252,   0,   0,   4,   0}, { 14, 318, 280, 332} }, /* 巨钳螳螂：剑舞 / 银色旋风 / 劈瓦 / 燕返 */
        {  62,  4, 11, 220, {252, 252,   0,   0,   0,   4}, {127, 280,  89,  95} }, /* 蚊香泳士：攀瀑 / 劈瓦 / 地震 / 催眠术 */
        { 160,  4, 67, 211, {  4, 252,   0,   0,   0, 252}, {349, 127,  89, 157} }, /* 大力鳄：  龙之舞 / 攀瀑 / 地震 / 岩崩 */
    },
    /* ---- 队 4：高速特攻队 ---- */
    {
        {  94, 11, 26, 224, {  4,   0,   0, 252,   0, 252}, {247, 188,  85,  95} }, /* 耿鬼：    暗影球 / 污泥炸弹 / 十万伏特 / 催眠术 */
        { 121, 11, 30, 220, {  4,   0,   0, 252,   0, 252}, { 57,  58,  85, 105} }, /* 宝石海星：冲浪 / 冰冻光束 / 十万伏特 / 自我再生 */
        { 135, 11, 10, 219, {  4,   0,   0, 252,   0, 252}, { 85, 247,  86,  97} }, /* 雷伊布：  十万伏特 / 暗影球 / 电磁波 / 高速移动 */
        {  65, 11, 39, 225, {  4,   0,   0, 252,   0, 252}, { 94, 347, 105,   7} }, /* 胡地：    精神强念 / 冥想 / 自我再生 / 火焰拳 */
        { 169, 14, 39, 221, {  4, 252,   0,   0,   0, 252}, {332, 188,  92, 109} }, /* 叉字蝠：  燕返 / 污泥炸弹 / 剧毒 / 奇异之光 */
        { 196, 11, 28, 209, {  4,   0,   0, 252,   0, 252}, { 94, 347, 234, 247} }, /* 太阳伊布：精神强念 / 冥想 / 晨光 / 暗影球 */
    },
    /* ---- 队 5：肉盾消耗队 ---- */
    {
        { 242,  6, 30, 211, {252,   0, 252,   0,   4,   0}, {135,  58,  85,  92} }, /* 幸福蛋：  生蛋 / 冰冻光束 / 十万伏特 / 剧毒 */
        { 143,  4, 17, 211, {252, 252,   0,   0,   4,   0}, { 34,  89, 156, 242} }, /* 卡比兽：  泰山压顶 / 地震 / 睡觉 / 咬碎 */
        { 245,  6, 46, 211, {252,   0, 252,   0,   4,   0}, { 57, 347, 156,  58} }, /* 水君：    冲浪 / 冥想 / 睡觉 / 冰冻光束 */
        { 208,  4,  5, 215, {252, 252,   0,   0,   4,   0}, { 89, 157, 231, 153} }, /* 大钢蛇：  地震 / 岩崩 / 铁尾 / 大爆炸 */
        { 197, 24, 28, 211, {252,   0,   0,   0, 252,   4}, { 92, 109, 236, 242} }, /* 月亮伊布：剧毒 / 奇异之光 / 月光 / 咬碎 */
        { 376,  4, 29, 211, {  4, 252,   0,   0,   0, 252}, {309,  89, 157, 153} }, /* 巨金怪：  彗星拳 / 地震 / 岩崩 / 大爆炸 */
    },
};

static const char *kPresetName[UI_PRESET_COUNT] = {
    "晴天队", "雨天队", "威吓物攻队", "高速特攻队", "肉盾消耗队"
};

static void load_side(Battle *b, int side_is_foe, const SesMonSpec *spec)
{
    for (int i = 0; i < TEAM_SIZE; i++) {
        BattleMon m;
        mon_init(&m, spec[i].pid, SES_DEFAULT_LEVEL, spec[i].nature,
                 spec[i].ability, spec[i].item, spec[i].moves);
        /* ev/iv 是 1-based；spec[i].ev 是 0 基 → +1 */
        for (int st = 0; st < 6; st++) {
            mon_set_ev(&m, (uint8_t)(st + 1), spec[i].ev[st]);
            mon_set_iv(&m, (uint8_t)(st + 1), SES_DEFAULT_IV);
        }
        /* mon_set_ev 只会在 HP 上限**变小**时夹 cur_hp；努力值把上限抬高后
         * 它不会补回来，不重置就会「满血开局的宝可梦其实是半血」。 */
        m.cur_hp = m.hp_max;
        battle_set_mon(b, side_is_foe, (uint8_t)i, &m);
    }
}

/*
 * 从五支预设队里给对手随机抽一支。
 *
 * 抽签**不消耗 battle_rng**：那是引擎的随机流，在队伍装载阶段动它会把
 * 整局对战的随机序列整体挪位，单测的可重放性就没了。这里用自己的会话级
 * 种子（Session.team_seed），真机阶段可由 main 用 esp_random() 播种 ——
 * 单测只要求「同一种子给同一支」，所以固定种子即可重放。
 */
static int draw_foe_preset(Session *s)
{
    s->team_seed = s->team_seed * 1103515245u + 12345u;
    return (int)((s->team_seed >> 16) % UI_PRESET_COUNT);
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
 * 「被击倒」的日志行。
 *
 * 这一类**不自动放过去**：停在原地等玩家按一次确定，由他自己决定什么时候
 * 翻过这一页（要不要换人、要不要看清是谁倒下的）。和「多停一拍的自动关键行」
 * 是两码事，所以单独一个谓词，不混进 log_dwell_for 的关键字表。
 *
 * **引擎里每一条「报死亡」的文案都必须命中这里的关键字**（现在这几种：
 * 被招式打死「… 倒下了！」、异常状态掉死「因 … 倒下了！」、混乱自伤
 * 「… 倒下了！」、道具反噬「被 … 反噬，倒下了！」、寄生种子吸干
 * 「被寄生种子吸干了！」）。以后新增死亡文案，这里要跟着加，否则那一种
 * 倒下就不会停下来等玩家。
 *
 * strstr 是逐字节匹配 UTF-8 —— 正是想要的：这些词都是完整词，不会落在
 * 多字节字符中间，所以子串匹配等价于词匹配。
 */
static bool log_is_ko(const char *t)
{
    return t && (strstr(t, "倒下") || strstr(t, "濒死") || strstr(t, "吸干了"));
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
    /* 整场对战的日志都自动推进（开场与回合内一视同仁）：玩家的手只用来出招，
     * 不用一条条按确定。只有「被击倒」那一行停下来等一次确定 —— 见 log_is_ko()。
     * 计时状态在这里重置，出发点是「未计时」。 */
    s->log_auto = !log_is_ko(ses_log_at(s, s->log_next));
    s->log_at_for = SES_LOG_AT_NONE;
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

/*
 * 当前这条日志该停多久（只对「会自动放过去」的行有意义）。
 *
 * 关键事件（天气/特性触发/起作用/威吓）多停一拍，让玩家看清发生了什么。
 * 「被击倒」的行不走这里 —— 它们由 log_is_ko() 管，停在原地等按键。
 * strstr 是逐字节匹配 UTF-8 —— 正是想要的：这些关键字都是完整词，
 * 不会落在多字节字符中间，所以子串匹配等价于词匹配。
 */
static uint32_t log_dwell_for(const Session *s)
{
    const char *t = ses_log_at(s, s->log_next);
    if (!t) {
        return SES_LOG_DWELL_MS;
    }
    static const char *const kKey[] = {"天气", "特性", "起作用", "威吓"};
    for (unsigned i = 0; i < sizeof(kKey) / sizeof(kKey[0]); i++) {
        if (strstr(t, kKey[i])) {
            return SES_LOG_DWELL_KEY_MS;
        }
    }
    return SES_LOG_DWELL_MS;
}

/*
 * 日志的受控自动推进：无按键时按停留时长逐条前进，直到遇上「被击倒」的行。
 *
 * 惰性起表：只有「本条真正成为当前条」之后才开始计时，这样中途被暂停
 * （key_down）或手动推进都不会把上一条的剩余时间带过来。
 */
static void ses_log_tick(Session *s, uint32_t now_ms)
{
    if (!s->log_auto) {
        return;                 /* 停在「被击倒」行/已被长按取消：只等手动推进 */
    }
    if (log_is_ko(ses_log_at(s, s->log_next))) {
        /* 被击倒：停下等玩家按一次确定。翻成 false 之后，状态行的等待动效
         * 与「确定   长按跳过」提示会自动跟着出来（见 ses_view）。 */
        s->log_auto = false;
        s->log_at_for = SES_LOG_AT_NONE;
        return;
    }
    if (s->key_down) {
        s->log_at_for = SES_LOG_AT_NONE;   /* 按住暂停；松开后重新计时 */
        return;
    }
    if (s->log_at_for != s->log_next) {    /* 惰性起表：本条到点后才开始计时 */
        s->log_at = now_ms;
        s->log_at_for = s->log_next;
        s->log_dwell_ms = log_dwell_for(s);
    }
    if (now_ms - s->log_at >= s->log_dwell_ms) {
        s->log_at_for = SES_LOG_AT_NONE;   /* 下一条重新起表 */
        log_step(s, 1);                    /* 到点前进一条（可能就此离开 LOG 阶段）*/
    }
}

static void enter_input(Session *s)
{
    s->log_auto = false;   /* 离开日志阶段就清掉自动标记，避免残留影响下一批 */
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
    s->log_auto = false;   /* 离开日志阶段就清掉自动标记，避免残留影响下一批 */
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
    s->log_auto = false;   /* 离开日志阶段就清掉自动标记，避免残留影响下一批 */
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

    case UI_ACT_PICK_TEAM:
        /* 玩家在「选择队伍」里选了一支：立刻装成我方队伍（对手重新抽）。
         * 界面停在列表上，长按返回主菜单后再「开始对战」。 */
        ses_load_preset(s, r->arg);
        refresh_model(s);
        return;

    case UI_ACT_PICK_SPECIES:
    case UI_ACT_NONE:
    default:
        /* 队伍编辑还没做，选完图鉴不做任何事 */
        return;
    }
}

/* ---------------------------------------------------------------- 对外 */

void ses_load_preset(Session *s, int idx)
{
    if (!s) {
        return;
    }
    if (idx < 0 || idx >= UI_PRESET_COUNT) {
        idx = 0;   /* 越界钳到第 0 支，绝不读表外 */
    }
    s->team_index = (uint8_t)idx;
    battle_init(&s->b);
    s->b.me.name = "我方";
    s->b.foe.name = "对手";
    load_side(&s->b, 0, kPreset[idx]);
    /* 对手从五支里随机抽一支（不占用引擎随机流） */
    load_side(&s->b, 1, kPreset[draw_foe_preset(s)]);
}

void ses_load_default_teams(Session *s)
{
    if (!s) {
        return;
    }
    ses_load_preset(s, s->team_index);
}

void ses_init(Session *s, uint8_t ai_level)
{
    if (!s) {
        return;
    }
    *s = (Session){ 0 };
    /* 整体清零后 log_at_for 会变成 0，而 0 是一个**合法序号** —— 不清成
     * 哨兵，惰性起表的第一帧就会误判成「已经在为第 0 条计时」而不起表。 */
    s->log_at_for = SES_LOG_AT_NONE;
    s->log_auto = false;
    s->key_down = false;
    s->wait_anim = 0;
    s->swallow_key = -1;
    s->team_seed = 0x9E3779B9u;   /* 非零起始，保证抽签序列有变化 */
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
     * 就是「用**玩家选的那支预设队**重新开始」—— ses_load_default_teams
     * 按 team_index 重装，保留选择。不复位的话第二局会带着上一局的残血和
     * 空 PP 开打。
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

    /* 维护「是否有键按着」：按住 = 暂停日志的自动推进。
     * 放在 swallow 分支之前，是因为长按跳过（LONG）之后 key_down 可能
     * 还没被 RELEASE 清掉，先维护更贴近真实电平。
     * **不能用 key_is_down()** —— 单测直接投事件、不走 key_poll，拿不到
     * 真实电平，只能靠事件自己记。 */
    if (msg->ev == KEY_EV_PRESS)   { s->key_down = true; }
    if (msg->ev == KEY_EV_RELEASE) { s->key_down = false; }

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
            s->log_at_for = SES_LOG_AT_NONE;
            log_step(s, 1);
            /* 每条都自动推进，手动按一下只是提前放 —— 所以放完这一条要重新
             * 起表（log_auto 恢复）。若新的一条又是「被击倒」，tick 会立刻再
             * 把它停下；若这一条已经是本批最后一条，log_step 已经离开日志
             * 阶段，就不用（也不该）再置了。 */
            if (s->phase == SES_PHASE_LOG) {
                s->log_auto = !log_is_ko(ses_log_at(s, s->log_next));
            }
        } else if (msg->ev == KEY_EV_LONG) {
            s->log_auto = false;   /* 长按 = 取消自动，剩余日志全跳过 */
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
    if (s->phase == SES_PHASE_LOG) {
        ses_log_tick(s, now_ms);
        /* 动效相位与计时无关，只要在 LOG 阶段就一直转（经 ses_view 输出，
         * 自动推进时是否画由 ses_view 自己按 log_auto 判断）。 */
        s->wait_anim = (int8_t)((now_ms / SES_WAIT_ANIM_MS) & 3);
        return;
    }
    if (s->phase != SES_PHASE_MAIN && s->phase != SES_PHASE_INPUT) {
        return;   /* 结算时不接受「按住连发」 */
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
    case UI_SCREEN_PRESET: {
        /* 预设队列表：第 i 项就是第 i 支队的名字 */
        if (index >= UI_PRESET_COUNT) {
            return NULL;
        }
        return kPresetName[index];
    }
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

    /* ---- HUD：当前天气（右上角标志）---- */
    switch (s->b.wx) {
    case WX_RAIN: out->wx_text = "雨"; break;
    case WX_SUN:  out->wx_text = "晴"; break;
    case WX_SAND: out->wx_text = "沙"; break;
    case WX_HAIL: out->wx_text = "雹"; break;
    default: /* WX_NONE 以及将来新增的天气：不画 */ break;
    }

    /* ---- HUD：我方携带道具（血条框内文字，不加图标）---- */
    if (me) {
        const ItemDef *it = item_get(me->base.item);
        out->my_item = (it && it->name_cn) ? it->name_cn : NULL;
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
        /* 播放日志时左侧报回合数：日志刷得太快，用户需要知道打到第几回合了。
         * 去空格是为了给状态行动效腾位置（3 位数时 61px + 起点 14 止于 75，
         * 动效画在 x=80）。
         * 右侧只在「停下等按键」时才提示要按 —— 也就是播到「被击倒」那一行
         * （log_auto 被 tick 翻成 false）；其余时间都在自动往下走。 */
        snprintf(s->status_left, sizeof(s->status_left), "第%u回合",
                 (unsigned)s->b.turn);
        snprintf(s->status_right, sizeof(s->status_right), "%s",
                 s->log_auto ? "自动中…" : "确定   长按跳过");
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
    /* ---- 等待动效：只在「手动推进」的日志里画；帧号取 1..4（0 = 不画）---- */
    out->wait_anim = (s->phase == SES_PHASE_LOG && !s->log_auto)
                     ? (int)s->wait_anim + 1 : 0;

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
    /* 带图标的三个列表用高行；其余屏（主菜单/弹层/认输/结算）保持默认 28 ——
     * 那些屏的取点断言是按 28 钉死的，不能被动到。 */
    out->row_h = (sc == UI_SCREEN_PRESET || sc == UI_SCREEN_TEAM
                  || sc == UI_SCREEN_SWITCH) ? MENU_ROW_H_ICON : 0;
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
     * 图鉴列表需要：386 项、一屏 8 行，不报序号的话根本不知道看的是第几只。
     * 预设队列表同理：5 支一屏放得下，但报出序号能看出当前光标在哪支。 */
    s->menu_left[0] = '\0';
    if ((sc == UI_SCREEN_TEAM || sc == UI_SCREEN_PRESET) && out->count > 0) {
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
        /* 图标屏的行首图标：图鉴列表 = 该图鉴物种；预设队列表 = 该队首发。 */
        if (sc == UI_SCREEN_TEAM) {
            out->row_icons[i] = icon_get((uint16_t)(out->top + i + 1));   /* 图鉴序号 = 下标+1 */
        } else if (sc == UI_SCREEN_PRESET) {
            int idx = out->top + i;
            if (idx >= 0 && idx < UI_PRESET_COUNT) {
                out->row_icons[i] = icon_get(kPreset[idx][0].pid);        /* 行首 = 该队首发宝可梦 */
            }
        }
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
            /* 图标用 base.species_id（真实物种），**不是 active_pid** ——
             * Mega 形态的 pid >= 10000，图标表只覆盖 1..386，用 active_pid 会查不到。 */
            out->row_icons[i] = icon_get(bm->base.species_id);
            out->row_hp_permil[i] = bm->hp_max
                ? (int)((uint32_t)bm->cur_hp * 1000 / bm->hp_max) : 0;
            snprintf(s->switch_lv[slot], SES_LV_TEXT_MAX, "Lv%u",
                     (unsigned)bm->base.level);
            out->subs[i] = s->switch_lv[slot];
            snprintf(s->menu_note[i], sizeof(s->menu_note[i]), "%u/%u",
                     (unsigned)bm->cur_hp, (unsigned)bm->hp_max);
            out->notes[i] = s->menu_note[i];
        }
    }

    return true;
}
