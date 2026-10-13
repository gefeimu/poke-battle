/*
 * poke_battle.c - 第六代 6v6 对战引擎实现
 *
 * 状态机只做三件事：
 *   1) 决定本回合谁先动
 *   2) 按顺序执行双方行动（出招 / 换人）
 *   3) 处理回合末效果与胜负判定
 *
 * 招式效果（EFFECT_*）尚未接入，当前所有招式都按"纯伤害"处理。
 * 效果分派是下一步，接口已经留在 apply_move_effects() 里。
 */

#include "poke_ability.h"
#include "poke_item.h"
#include "poke_battle.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

/* ---------- 随机源 ---------- */

/*
 * 单测提供同名强符号覆盖本实现，把随机结果固定下来。
 * 真机上可改用 esp_random() 或硬件 RNG。
 *
 * s_rng 无条件定义（即使测试覆盖了 battle_rng），是为了让
 * battle_rng_state() 在任何构建下都能编译：AI 搜索靠它
 * 保存/恢复随机流，避免搜索消耗真实对局的熵。
 */
static uint32_t s_rng = 0x9E3779B9u;

#ifndef POKE_BATTLE_TEST_NO_RNG
uint32_t battle_rng(void)
{
    s_rng ^= s_rng << 13;
    s_rng ^= s_rng >> 17;
    s_rng ^= s_rng << 5;
    return s_rng;
}
#endif

uint32_t *battle_rng_state(void)
{
    return &s_rng;
}

/* ---------- 日志 ---------- */

/*
 * 日志静音开关。AI 搜索会在克隆状态上跑几百次 battle_step，
 * 那些日志没人看；ESP32 上 vsnprintf 很贵（soft-float newlib），
 * 静音后模拟速度快数倍。真机对局的日志保持开启。
 */
static bool s_log_enabled = true;

void battle_log_enable(bool on)
{
    s_log_enabled = on;
}

bool battle_log_enabled(void)
{
    return s_log_enabled;
}

void log_init(BattleLog *l)
{
    memset(l, 0, sizeof(*l));
}

void log_add(BattleLog *l, const char *fmt, ...)
{
    if (!l || !s_log_enabled) {
        return;
    }
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(l->line[l->head], sizeof(l->line[0]), fmt, ap);
    va_end(ap);
    l->head = (uint8_t)((l->head + 1) % MAX_LOG);
    l->count++;
}

const char *log_get(const BattleLog *l, uint16_t i)
{
    if (!l || i >= MAX_LOG || i >= l->count) {
        return NULL;
    }
    /* count 可能超过 MAX_LOG，此时最早的记录已被覆盖 */
    uint16_t start = (l->count > MAX_LOG) ? l->head : 0;
    return l->line[(uint16_t)((start + i) % MAX_LOG)];
}

/* ---------- 小工具 ---------- */

static const char *mon_name(const BattleMon *m)
{
    const SpeciesDef *sd = species_get(m->active_pid);
    return sd ? sd->name : "???";
}

static const char *atk_side_name(const Battle *b, int is_foe)
{
    return is_foe ? b->foe.name : b->me.name;
}

/* 实际速度：能力等级 + 麻痹减速 */
/*
 * 实际速度。按官方顺序叠加修正：
 *   基础值 -> 能力等级 -> 麻痹 0.5 -> 特性（叶绿素/悠游自如）-> 天气
 * 天气对速度的影响已由特性覆盖（叶绿素只在晴天使速翻倍），
 * 所以这里只需传当前天气给特性查询。
 */
static uint16_t effective_speed(const Battle *b, const BattleMon *m)
{
    uint32_t s = calc_stat_staged(&m->base, 6, m->stage[5]);
    if (m->cond == CON_PARALYZE) {
        s = s / 2; /* 麻痹速度减半（Gen III 起一直是 0.5） */
    }
    s = s * ability_speed_mult(m, m->base.ability, b->wx) / 100;
    if (s < 1) {
        s = 1;
    }
    return (uint16_t)s;
}

/* ---------- 装配 ---------- */

void mon_init(BattleMon *m, uint16_t species_id, uint8_t level,
              uint8_t nature, uint8_t ability, uint16_t item,
              const uint16_t moves[4])
{
    memset(m, 0, sizeof(*m));
    statsheet_init(&m->base, species_id, level ? level : 50, nature);
    m->base.ability = ability;
    m->base.item = item;
    m->active_pid = species_id;
    m->hp_max = calc_hp_max(&m->base);
    m->cur_hp = m->hp_max;
    m->alive = true;
    if (moves) {
        for (int i = 0; i < 4; i++) {
            m->move[i] = moves[i];
            /* 索引约定在下标 = id-1，务必走 mv_get 而不是裸下标 */
            const MoveDef *md = mv_get(moves[i]);
            if (md) {
                m->pp_max[i] = md->pp ? md->pp : 1;
                m->pp[i] = m->pp_max[i];
            } else {
                m->move[i] = 0; /* id 非法，当空槽处理 */
            }
        }
    }
}

void mon_set_ev(BattleMon *m, uint8_t stat_idx, uint8_t value)
{
    statsheet_set_ev(&m->base, stat_idx, value);
    m->hp_max = calc_hp_max(&m->base);
    if (m->cur_hp > m->hp_max) {
        m->cur_hp = m->hp_max;
    }
}

void mon_set_iv(BattleMon *m, uint8_t stat_idx, uint8_t value)
{
    /* stat_idx 是 1-based（1=hp），0 会让 iv[-1] 写到相邻字段
     * （实测正好落在 base.mega，静默破坏 Mega 还原），必须挡下界。 */
    if (stat_idx < 1 || stat_idx > 6) {
        return;
    }
    m->base.ey.iv[stat_idx - 1] = value > MAX_IV ? MAX_IV : value;
    m->hp_max = calc_hp_max(&m->base);
    if (m->cur_hp > m->hp_max) {
        m->cur_hp = m->hp_max;
    }
}

/* ---------- 战斗流程 ---------- */

void battle_init(Battle *b)
{
    memset(b, 0, sizeof(*b));
    log_init(&b->log);
    b->result = BATTLE_ONGOING;
    b->my_action = 0xFF; /* 默认换人，防止未初始化时误出招 */
    b->foe_action = 0xFF;
}

int battle_set_mon(Battle *b, int side_is_foe, uint8_t slot, const BattleMon *m)
{
    if (slot >= TEAM_SIZE || !m) {
        return -1;
    }
    BattleSide *s = side_is_foe ? &b->foe : &b->me;
    s->team[slot] = *m;
    /* 首发默认用 0 号位，调用方可在 battle_start 前改 s->lead */
    return 0;
}

void battle_start(Battle *b)
{
    b->turn = 1;
    b->wx = WX_NONE;
    b->wx_turns = 0;
    log_add(&b->log, "战斗开始！%s 对 %s", b->me.name, b->foe.name);
    log_add(&b->log, "%s 派出了 %s！", b->me.name,
            mon_name(battle_active(b, 0)));
    log_add(&b->log, "%s 派出了 %s！", b->foe.name,
            mon_name(battle_active(b, 1)));
    /* 出场特性（威吓 / 天气） */
    ability_on_switch_in(b, 0);
    ability_on_switch_in(b, 1);
}

BattleMon *battle_active(Battle *b, int side_is_foe)
{
    BattleSide *s = side_is_foe ? &b->foe : &b->me;
    if (s->active >= TEAM_SIZE) {
        return NULL;
    }
    return &s->team[s->active];
}

const BattleMon *battle_active_c(const Battle *b, int side_is_foe)
{
    const BattleSide *s = side_is_foe ? &b->foe : &b->me;
    if (s->active >= TEAM_SIZE) {
        return NULL;
    }
    return &s->team[s->active];
}

int battle_has_alive(const Battle *b, int side_is_foe)
{
    const BattleSide *s = side_is_foe ? &b->foe : &b->me;
    for (int i = 0; i < TEAM_SIZE; i++) {
        if (s->team[i].alive && s->team[i].cur_hp > 0) {
            return 1;
        }
    }
    return 0;
}

uint16_t mon_pp_left(const BattleMon *m)
{
    uint16_t n = 0;
    for (int i = 0; i < 4; i++) {
        n += m->pp[i];
    }
    return n;
}

int mon_all_pp_gone(const BattleMon *m)
{
    for (int i = 0; i < 4; i++) {
        if (m->move[i] && m->pp[i] > 0) {
            return 0;
        }
    }
    return 1;
}

/* ---------- 内部：执行一次攻击 ---------- */

/* ---------- 效果分派 ---------- */

/* 能力名与升降文案。等级 1/2/3 对应"提高/大幅提高/极大提高" */
static const char *STAT_NAME_CN[] = {
    "HP", "攻击", "防御", "特攻", "特防", "速度", "命中率", "闪避率"
};
static const char *RAISE_CN[] = {"", "提高了", "大幅提高了", "极大提高了"};
static const char *FALL_CN[] = {"", "降低了", "大幅降低了", "极大降低了"};

/* 能力等级变化的实际写入。返回 1 表示有变化、0 表示已到上限/下限 */
static int apply_stat_change(BattleMon *m, uint8_t stat, int8_t delta,
                             BattleLog *log, const char *who)
{
    if (stat < 1 || stat > 8) {
        return 0;
    }
    /* stat 1..6 走 stage[]，7=命中 8=闪避单独存 */
    int8_t *slot = (stat <= 6) ? &m->stage[stat - 1]
                               : (stat == 7 ? &m->stage_acc : &m->stage_eva);
    int old = *slot;
    int nv = old + delta;
    if (nv > MAX_STAGE) {
        nv = MAX_STAGE;
    }
    if (nv < -MAX_STAGE) {
        nv = -MAX_STAGE;
    }
    if (nv == old) {
        log_add(log, "%s 的 %s 已经无法再%s了", who, STAT_NAME_CN[stat - 1],
                delta > 0 ? "提高" : "降低");
        return 0;
    }
    *slot = (int8_t)nv;
    int lvl = delta > 0 ? delta : -delta;
    if (lvl > 3) {
        lvl = 3;
    }
    log_add(log, "%s 的 %s %s！", who, STAT_NAME_CN[stat - 1],
            delta > 0 ? RAISE_CN[lvl] : FALL_CN[lvl]);
    return 1;
}

/* 异常状态施加。返回 1 成功、0 失败（已免疫/已有状态）。
 * move_type 是招式属性：电系麻痹招（电磁波）对地面属性无效。 */
static int apply_ailment(BattleMon *m, uint8_t ailment, uint8_t move_type,
                         BattleLog *log, const char *who)
{
    switch (ailment) {
        case MAIL_PARALYSIS:
            if (m->cond != CON_NONE) {
                return 0;
            }
            /* 电系麻痹对地面无效（第六代规则，电磁波打不动地面） */
            if (move_type == TYPE_ELECTRIC) {
                const SpeciesDef *sd = species_get(m->active_pid);
                if (sd && (sd->type1 == TYPE_GROUND || sd->type2 == TYPE_GROUND)) {
                    log_add(log, "对 %s 没有效果……", who);
                    return 0;
                }
            }
            m->cond = CON_PARALYZE;
            log_add(log, "%s 麻痹了，很难使出招式！", who);
            return 1;
        case MAIL_SLEEP:
            if (m->cond != CON_NONE) {
                return 0;
            }
            m->cond = CON_SLEEP;
            m->sleep_turns = (uint8_t)(1 + (battle_rng() % 3)); /* 1~3 回合 */
            log_add(log, "%s 睡着了！", who);
            return 1;
        case MAIL_FREEZE:
            if (m->cond != CON_NONE) {
                return 0;
            }
            m->cond = CON_FREEZE;
            log_add(log, "%s 被冻住了！", who);
            return 1;
        case MAIL_BURN:
            if (m->cond != CON_NONE) {
                return 0;
            }
            m->cond = CON_BURN;
            log_add(log, "%s 被灼伤了！", who);
            return 1;
        case MAIL_POISON:
            if (m->cond != CON_NONE) {
                return 0;
            }
            m->cond = CON_POISON;
            log_add(log, "%s 中毒了！", who);
            return 1;
        case MAIL_CONFUSION:
            if (m->confused) {
                return 0;
            }
            m->confused = (uint8_t)(2 + (battle_rng() % 4)); /* 2~5 回合 */
            log_add(log, "%s 混乱了！", who);
            return 1;
        case MAIL_LEECH_SEED:
            if (m->leech_seeded) {
                return 0;
            }
            /* 草系免疫寄生种子 */
            const SpeciesDef *sd = species_get(m->active_pid);
            if (sd && (sd->type1 == TYPE_GRASS || sd->type2 == TYPE_GRASS)) {
                log_add(log, "对 %s 没有效果……", who);
                return 0;
            }
            m->leech_seeded = 1;
            log_add(log, "%s 被种下了寄生种子！", who);
            return 1;
        default:
            /* 其他异常（着迷/咒术/封印...）尚未实现 */
            return 0;
    }
}

/*
 * 招式效果主分派。在伤害结算后调用。
 *
 * 覆盖三代 90% 以上的常用招式：
 *   能力变化 / 异常状态 / 回复 / 吸取 / 多段 / 畏缩 / 天气 / 强制换人
 * 未实现：一击必杀、蓄力两回合、替身、变身、场地、全场效果等。
 */
static void apply_move_effects(Battle *b, int attacker_is_foe,
                               const MoveDef *mv, uint16_t dmg_dealt)
{
    BattleSide *atk_side = attacker_is_foe ? &b->foe : &b->me;
    BattleSide *def_side = attacker_is_foe ? &b->me : &b->foe;
    BattleMon *atk = &atk_side->team[atk_side->active];
    BattleMon *def = &def_side->team[def_side->active];
    const char *an = atk_side->name;
    const char *dn = def_side->name;
    BattleLog *log = &b->log;

    /* ---- 异常状态 ----
     * 两个类别语义不同，不能都用 ailment_chance 判断：
     *   MCAT_AILMENT       纯异常招（电磁波/催眠粉）：必定触发，
     *                      Veekun 把这类记 ailment_chance=0
     *   MCAT_DAMAGE_AILMENT 伤害附异常（火焰拳/毒针）：按 chance 概率
     *   MCAT_SWAGGER       也是必定异常（混乱）
     * 曾把两类混为一个条件，导致电磁波这类必定触发的招式永远不生效。 */
    if (mv->ailment != MAIL_NONE) {
        int must = (mv->category == MCAT_AILMENT ||
                    mv->category == MCAT_SWAGGER);
        if (must) {
            apply_ailment(def, mv->ailment, mv->type, log, dn);
        } else if (mv->ailment_chance > 0 && dmg_dealt > 0) {
            if ((int)(battle_rng() % 100) < mv->ailment_chance) {
                apply_ailment(def, mv->ailment, mv->type, log, dn);
            }
        }
    }

    /* ---- 能力变化 ---- */
    if (mv->n_stat_changes > 0) {
        unsigned chance = mv->stat_chance ? mv->stat_chance : 100u;
        if ((battle_rng() % 100u) < chance) {
            /* 自身提升类（MCAT_NET_GOOD_STATS）作用于自己，
             * 伤害附带的变化按招式方向：降对方 / 升自己 */
            int to_self = (mv->category == MCAT_NET_GOOD_STATS ||
                           mv->category == MCAT_DAMAGE_RAISE);
            for (unsigned i = 0; i < mv->n_stat_changes; i++) {
                const StatChange *sc = &mv->stat_changes[i];
                if (sc->change == 0) {
                    continue;
                }
                if (to_self) {
                    apply_stat_change(atk, sc->stat, sc->change, log, an);
                } else {
                    /* 对方能力变化：招式的 change 是负值表示降低 */
                    apply_stat_change(def, sc->stat, sc->change, log, dn);
                }
            }
        }
    }

    /* ---- 回复 ---- */
    if (mv->category == MCAT_HEAL && mv->heal > 0) {
        uint16_t amount = atk->hp_max * mv->heal / 100;
        if (atk->cur_hp >= atk->hp_max) {
            log_add(log, "%s 的 HP 已经全满了！", an);
        } else {
            atk->cur_hp = (uint16_t)(atk->cur_hp + amount);
            if (atk->cur_hp > atk->hp_max) {
                atk->cur_hp = atk->hp_max;
            }
            log_add(log, "%s 回复了 %u 点 HP！", an, amount);
        }
    }

    /* ---- 吸取（伤害的 drain%） ---- */
    if (mv->drain > 0 && dmg_dealt > 0) {
        uint16_t amount = dmg_dealt * mv->drain / 100;
        if (amount == 0) {
            amount = 1;
        }
        atk->cur_hp = (uint16_t)(atk->cur_hp + amount);
        if (atk->cur_hp > atk->hp_max) {
            atk->cur_hp = atk->hp_max;
        }
        log_add(log, "%s 吸取了 %u 点 HP！", an, amount);
    }

    /* ---- 天气 ---- */
    if (mv->category == MCAT_WHOLE_FIELD || mv->category == MCAT_FIELD) {
        Weather w = WX_NONE;
        const char *wn = "";
        if (mv->type == TYPE_WATER) {
            w = WX_RAIN;
            wn = "开始下雨了！";
        } else if (mv->type == TYPE_FIRE) {
            w = WX_SUN;
            wn = "阳光变得强烈了！";
        } else if (mv->type == TYPE_ROCK) {
            w = WX_SAND;
            wn = "沙暴刮起来了！";
        } else if (mv->type == TYPE_ICE) {
            w = WX_HAIL;
            wn = "开始下冰雹了！";
        }
        if (w != WX_NONE) {
            b->wx = w;
            /* 回合末会先减一次，设 6 使实际生效5 回合（见 end_of_turn 注释） */
            b->wx_turns = 6;
            log_add(log, "%s", wn);
        }
    }
}

/*
 * 出招。返回 1 表示这一下把对手打倒了。
 *
 * 尚未处理：多段攻击、命中判定（miss）、效果命中、特性、道具、
 * 连续技、蓄力、吸收、回复、天气/场地招、先制招的特殊规则。
 */
static int do_move(Battle *b, int attacker_is_foe)
{
    BattleSide *atk_side = attacker_is_foe ? &b->foe : &b->me;
    BattleSide *def_side = attacker_is_foe ? &b->me : &b->foe;
    BattleMon *atk = &atk_side->team[atk_side->active];
    BattleMon *def = &def_side->team[def_side->active];

    uint8_t slot = attacker_is_foe ? b->foe_action : b->my_action;
    if (slot >= 4 || !atk->move[slot]) {
        log_add(&b->log, "%s 什么都做不了", atk_side->name);
        return 0;
    }
    /* 索引约定在下标 = id-1，务必走 mv_get 而不是裸下标 */
    const MoveDef *mv = mv_get(atk->move[slot]);
    if (!mv) {
        log_add(&b->log, "%s 什么都做不了", atk_side->name);
        return 0;
    }

    if (atk->pp[slot] == 0) {
        log_add(&b->log, "%s 的 %s 没有 PP 了！", atk_side->name, mv->name);
        return 0;
    }
    atk->pp[slot]--;

    log_add(&b->log, "%s 使用了 %s！", atk_side->name, mv->name);

    /* ---- 命中判定 ---- */
    /* accuracy 0 = 必中。命中率受双方命中/闪避等级影响（每级 ±0.33 倍）。 */
    if (mv->accuracy && mv->accuracy < 100) {
        int acc = mv->accuracy;
        int acc_stage = atk->stage_acc - def->stage_eva;
        if (acc_stage > 0) {
            acc = acc * (3 + acc_stage) / 3;
        } else if (acc_stage < 0) {
            acc = acc * 3 / (3 - acc_stage);
        }
        if (acc > 100) {
            acc = 100;
        }
        if ((int)(battle_rng() % 100) >= acc) {
            log_add(&b->log, "%s 的攻击落空了！", atk_side->name);
            return 0;
        }
    }

    /* ---- 状态招式 ---- */
    if (mv->power == 0) {
        apply_move_effects(b, attacker_is_foe, mv, 0);
        return 0;
    }

    /* ---- 属性免疫（飘浮/储水/蓄电/引火等） ---- */
    if (ability_immune_type(def, def->base.ability, mv->type)) {
        log_add(&b->log, "%s 的 %s 不受 %s 影响！", def_side->name,
                ability_name(def->base.ability), mv->name);
        return 0;
    }

    /* ---- 伤害招式 ---- */
    /* 暴击：第六代基础 1/16，招式 crit_rate 每级翻倍 */
    int crit_stage = (mv->crit_rate >= 1) ? mv->crit_rate : 0;
    crit_stage += item_crit_stage(atk->base.item);
    if (crit_stage > 4) {
        crit_stage = 4; /* 上限 4 级（约 50%） */
    }
    int crit_pct = crit_stage > 0 ? (63 * (1 << crit_stage)) : 63;
    if ((int)(battle_rng() % 1000) < crit_pct) {
        crit_pct = -1; /* 强制暴击 */
    }

    /* 多段攻击：按 min_hits..max_hits 均匀抽次数 */
    int hits = 1;
    if (mv->max_hits > 1) {
        int span = mv->max_hits - mv->min_hits + 1;
        hits = mv->min_hits + (int)(battle_rng() % (unsigned)span);
        if (hits < 1) {
            hits = 1;
        }
    }

    /* 特性威力修正（猛火/适应力/技师等）。
     * calc_damage_ex 不接特性参数，这里把修正乘进一个临时 MoveDef，
     * 避免改动 calc 层签名（calc 层保持纯计算、可独立单测）。 */
    MoveDef eff_mv = *mv;
    /* 特性威力修正（猛火/适应力/技师） */
    uint16_t pw_mult = ability_power_mult(atk, atk->base.ability, mv);
    /* 道具威力修正（木炭/黑带等属性增强类） */
    pw_mult = (uint16_t)(pw_mult * item_type_boost(atk->base.item, mv->type) / 100);
    if (pw_mult != 100) {
        uint32_t p = (uint32_t)mv->power * pw_mult / 100;
        eff_mv.power = (uint8_t)(p > 255 ? 255 : p);
    }

    uint16_t total = 0;
    for (int i = 0; i < hits; i++) {
        int did_crit = 0;
        uint16_t dmg = calc_damage_ex(&atk->base, atk->cond, atk->stage,
                                      &def->base, def->cond, def->stage,
                                      &eff_mv, b->wx, (int16_t)crit_pct, &did_crit);
        if (dmg == 0) {
            if (hits == 1) {
                log_add(&b->log, "对 %s 没有效果……", mon_name(def));
                return 0;
            }
            break;
        }
        if (did_crit && i == 0) {
            log_add(&b->log, "会心一击！");
        }
        total = (uint16_t)(total + dmg);
        if (total > 999) {
            total = 999;
        }
        if (def->cur_hp <= dmg) {
            def->cur_hp = 0;
            def->alive = false;
            break;
        }
        def->cur_hp = (uint16_t)(def->cur_hp - dmg);
    }

    if (hits > 1) {
        log_add(&b->log, "造成了 %u 点伤害（%d 连击）", total, hits);
    } else {
        log_add(&b->log, "造成了 %u 点伤害", total);
    }

    /* ---- 效果分派（在伤害之后） ---- */
    if (def->alive) {
        apply_move_effects(b, attacker_is_foe, mv, total);
    }

    /* ---- 道具：贝壳之铃回血 ---- */
    item_on_deal_damage(b, attacker_is_foe, total);

    /* ---- 畏缩 ---- */
    if (mv->flinch_chance > 0 && def->alive) {
        if ((int)(battle_rng() % 100) < mv->flinch_chance) {
            def->flinched = 1;
        }
    }

    return def->cur_hp == 0 ? 1 : 0;
}

/* ---------- 内部：回合末 ---------- */

static void end_of_turn(Battle *b)
{
    /* 特性回合末（加速等） */
    ability_on_end_turn(b, 0);
    ability_on_end_turn(b, 1);
    /* 道具回合末（吃剩的东西） */
    item_on_end_turn(b, 0);
    item_on_end_turn(b, 1);

    /*
 * 天气倒计时。
 *
 * 递减发生在回合末，而设天气也发生在回合内（招式 / 特性），
 * 所以设天气的当回合就会被减一次。若设 5，实际只生效 4 回合。
 * 这里设 6 作为缓冲，使实际生效回合数 = 5，与第六代规则一致。
 */
    if (b->wx_turns > 0) {
        b->wx_turns--;
        if (b->wx_turns == 0) {
            log_add(&b->log, "天气恢复正常了");
            b->wx = WX_NONE;
        }
    }

    for (int side = 0; side < 2; side++) {
        BattleSide *s = side ? &b->foe : &b->me;
        BattleMon *m = &s->team[s->active];
        if (!m->alive) {
            continue;
        }
        /* 寄生种子：每回合末被吸 1/8，吸给对方（若对方在场） */
        if (m->leech_seeded && m->cur_hp > 0) {
            BattleMon *host = battle_active(b, !side);
            uint16_t d = m->hp_max / 8;
            if (d < 1) {
                d = 1;
            }
            if (d >= m->cur_hp) {
                m->cur_hp = 0;
                m->alive = false;
                log_add(&b->log, "%s 的 %s 被寄生种子吸干了！", s->name, mon_name(m));
            } else {
                m->cur_hp = (uint16_t)(m->cur_hp - d);
                if (host && host->alive) {
                    host->cur_hp = (uint16_t)(host->cur_hp + d);
                    if (host->cur_hp > host->hp_max) {
                        host->cur_hp = host->hp_max;
                    }
                    log_add(&b->log, "%s 的 HP 被寄生种子吸走了 %u 点", s->name, d);
                }
            }
        }

        /* 毒/灼伤每回合末掉 1/8 */
        if (m->cond == CON_POISON || m->cond == CON_BURN) {
            uint16_t d = m->hp_max / 8;
            if (d < 1) {
                d = 1;
            }
            if (d >= m->cur_hp) {
                m->cur_hp = 0;
                m->alive = false;
                log_add(&b->log, "%s 因 %s 倒下了！", s->name, mon_name(m));
            } else {
                m->cur_hp = (uint16_t)(m->cur_hp - d);
            }
        }
    }
}

static void check_battle_end(Battle *b)
{
    if (b->result != BATTLE_ONGOING) {
        return;
    }
    int me_alive = battle_has_alive(b, 0);
    int foe_alive = battle_has_alive(b, 1);
    if (!me_alive && !foe_alive) {
        b->result = BATTLE_DRAW;
    } else if (!me_alive) {
        b->result = BATTLE_LOSE;
    } else if (!foe_alive) {
        b->result = BATTLE_WIN;
    }
}

/* ---------- 内部：换人 ---------- */

static void clear_volatile(BattleMon *m)
{
    memset(m->stage, 0, sizeof(m->stage));
    m->stage_acc = 0;
    m->stage_eva = 0;
    m->sleep_turns = 0;
    /*
     * 睡眠必须连cond 一起清。只清 sleep_turns 会留下
     * cond == CON_SLEEP 且 sleep_turns == 0 的僵死态：
     * 行动时的睡眠分支要求 sleep_turns > 0 所以不成立（能行动），
     * 但 apply_ailment 每个分支都以 cond != CON_NONE 开头，
     * 于是这只再也挨不上任何异常，UI 还一直显示「睡眠中」。
     * 中毒/灼伤按官方规则要保留到战斗结束或换人，不在此清。
     */
    if (m->cond == CON_SLEEP) {
        m->cond = CON_NONE;
    }
    m->confused = 0;
    m->flinched = 0;
    m->leech_seeded = 0;
    m->protect = 0;
}

int battle_switch(Battle *b, int side_is_foe, uint8_t slot)
{
    BattleSide *s = side_is_foe ? &b->foe : &b->me;
    if (slot >= TEAM_SIZE) {
        return -1;
    }
    BattleMon *m = &s->team[slot];
    if (!m->alive || m->cur_hp == 0) {
        return -1;
    }
    if (s->active == slot) {
        return 0;
    }
    BattleMon *old = &s->team[s->active];
    clear_volatile(old);
    s->active = slot;
    clear_volatile(m);
    log_add(&b->log, "%s 收回了 %s，派出了 %s！", s->name, mon_name(old),
            mon_name(m));
    /* 出场特性（威吓/天气等）在换人落地时触发，与游戏一致。
     * 放在 battle_switch 里而不是 battle_step，是为了让
     * "濒死强制换人"这条路径也能触发。 */
    ability_on_switch_in(b, side_is_foe);
    return 0;
}

int battle_mega(Battle *b, int side_is_foe, uint16_t mega_pid)
{
    BattleMon *m = battle_active(b, side_is_foe);
    if (!m || m->mega_done) {
        return -1;
    }
    if (mega_to_base(mega_pid) != m->active_pid) {
        return -1; /* 不是这只的 Mega 石 */
    }
    /* 还必须真的带着对应的 Mega 石，否则任何队内携带任意石头的
     * 宝可梦都能进化，AI 也会基于错误前提做决策 */
    if (!mon_has_mega_stone(m)) {
        return -1;
    }
    statsheet_set_mega(&m->base, mega_pid);
    m->active_pid = mega_pid;
    m->mega_done = 1;
    log_add(&b->log, "%s 的 %s 超级进化了！",
            side_is_foe ? b->foe.name : b->me.name, mon_name(m));
    return 0;
}

/* ---------- 内部：出手顺序 ---------- */

/* 返回 1 表示 attacker 先动 */
static int who_first(Battle *b, int me_switches, int foe_switches)
{
    /* 换人方后动；双方都换人则按原本顺序 */
    if (me_switches && !foe_switches) {
        return 0;
    }
    if (foe_switches && !me_switches) {
        return 1;
    }

    const BattleMon *me = battle_active_c(b, 0);
    const BattleMon *foe = battle_active_c(b, 1);

    /* 先制招式优先。索引约定在下标 = id-1，走 mv_get。 */
int8_t p_me = 0, p_foe = 0;
if (b->my_action < 4 && me->move[b->my_action]) {
        const MoveDef *mv = mv_get(me->move[b->my_action]);
        p_me = mv ? (int8_t)mv->priority : 0;
    }
    if (b->foe_action < 4 && foe->move[b->foe_action]) {
        const MoveDef *mv = mv_get(foe->move[b->foe_action]);
        p_foe = mv ? (int8_t)mv->priority : 0;
    }
    if (p_me != p_foe) {
        return p_me > p_foe ? 1 : 0;
    }

    uint16_t sp_me = effective_speed(b, me);
    uint16_t sp_foe = effective_speed(b, foe);
    if (sp_me != sp_foe) {
        return sp_me > sp_foe ? 1 : 0;
    }
    /* 同速随机 */
    return (battle_rng() & 1) ? 1 : 0;
}

/* ---------- 主流程 ---------- */

int battle_step(Battle *b)
{
    if (b->result != BATTLE_ONGOING) {
        return -1;
    }
    if (!battle_has_alive(b, 0) || !battle_has_alive(b, 1)) {
        check_battle_end(b);
        return -1;
    }

    /* 防呆护栏：出战宝可梦已濒死而调用方尚未换人时，拒绝推进回合，
     * 直接返回需要换人的信号。否则双方出战位都判 !alive 而双双
     * continue，整回合静默空转，战斗会卡到回合上限判平局
     * （基准测试曾因漏处理返回值而 200 局全平）。 */
    BattleMon *guard_me = battle_active(b, 0);
    BattleMon *guard_foe = battle_active(b, 1);
    if ((guard_me && guard_me->cur_hp == 0) || (guard_foe && guard_foe->cur_hp == 0)) {
        b->need_switch_me = (uint8_t)(guard_me && guard_me->cur_hp == 0);
        b->need_switch_foe = (uint8_t)(guard_foe && guard_foe->cur_hp == 0);
        if (b->need_switch_me && b->need_switch_foe) {
            return 3;
        }
        return b->need_switch_me ? 1 : 2;
    }

    int me_switch = (b->my_action == 0xFF);
    int foe_switch = (b->foe_action == 0xFF);

    log_add(&b->log, "--- 第 %u 回合 ---", b->turn);

    /* 守住只持续一回合，回合开始清掉上一回合的标记 */
    battle_active(b, 0)->protect = 0;
    battle_active(b, 1)->protect = 0;

    /* 换人先执行（换人本身消耗回合） */
    if (me_switch) {
        if (b->my_switch < TEAM_SIZE) {
            battle_switch(b, 0, b->my_switch);
        }
    }
    if (foe_switch) {
        if (b->foe_switch < TEAM_SIZE) {
            battle_switch(b, 1, b->foe_switch);
        }
    }

    int me_first = who_first(b, me_switch, foe_switch);

    for (int pass = 0; pass < 2; pass++) {
        int attacker_is_foe = (pass == 0) ? (me_first ? 0 : 1)
                                          : (me_first ? 1 : 0);
        BattleMon *atk = battle_active(b, attacker_is_foe);
        BattleMon *def = battle_active(b, !attacker_is_foe);
        if (!atk->alive || !def->alive) {
            continue;
        }
        if ((attacker_is_foe ? foe_switch : me_switch)) {
            continue; /* 换人方不出招 */
        }

        /* 睡眠：不能出招 */
        if (atk->cond == CON_SLEEP && atk->sleep_turns > 0) {
            atk->sleep_turns--;
            log_add(&b->log, "%s 正在睡觉，无法行动", atk_side_name(b, attacker_is_foe));
            if (atk->sleep_turns == 0) {
                atk->cond = CON_NONE;
                log_add(&b->log, "%s 醒过来了！", atk_side_name(b, attacker_is_foe));
            }
            continue;
        }
        /* 冰冻：20% 解冻，否则不能动 */
        if (atk->cond == CON_FREEZE) {
            if (battle_rng() % 100 < 20) {
                atk->cond = CON_NONE;
                log_add(&b->log, "%s 的冰融化了！", atk_side_name(b, attacker_is_foe));
            } else {
                log_add(&b->log, "%s 被冻住了，无法动弹",
                        atk_side_name(b, attacker_is_foe));
                continue;
            }
        }
        /* 麻痹：25% 不能动 */
        if (atk->cond == CON_PARALYZE && (battle_rng() % 100) < 25) {
            log_add(&b->log, "%s 因麻痹无法动弹！", atk_side_name(b, attacker_is_foe));
            continue;
        }
        /* 混乱：33% 概率打自己，命中率为固定值 */
        if (atk->confused) {
            atk->confused--;
            if (atk->confused == 0) {
                log_add(&b->log, "%s 的混乱解除了！", atk_side_name(b, attacker_is_foe));
            } else if ((battle_rng() % 100) < 33) {
                /* 用 40 威力的物理招打自己，不吃 STAB 与克制 */
                uint32_t A = calc_stat_staged(&atk->base, 2, atk->stage[1]);
                uint32_t D = calc_stat_staged(&atk->base, 3, atk->stage[2]);
                uint16_t dmg = (uint16_t)(((2 * atk->base.level / 5 + 2) * A * 40 / 50 / (D ? D : 1)) + 2);
                if (dmg >= atk->cur_hp) {
                    atk->cur_hp = 0;
                    atk->alive = false;
                    log_add(&b->log, "%s 在混乱中打了自己，倒下了！",
                            atk_side_name(b, attacker_is_foe));
                } else {
                    atk->cur_hp = (uint16_t)(atk->cur_hp - dmg);
                    log_add(&b->log, "%s 在混乱中打了自己，受到 %u 点伤害！",
                            atk_side_name(b, attacker_is_foe), dmg);
                }
                continue;
            }
        }

        /* 畏缩 */
        if (atk->flinched) {
            atk->flinched = 0;
            log_add(&b->log, "%s 畏缩了，无法出招！", atk_side_name(b, attacker_is_foe));
            continue;
        }

        do_move(b, attacker_is_foe);
    }

    /* 回合末 */
    end_of_turn(b);
    check_battle_end(b);

    b->turn++;
    if (b->turn > MAX_TURNS && b->result == BATTLE_ONGOING) {
        b->result = BATTLE_DRAW;
        log_add(&b->log, "达到回合上限，判平局");
    }

    /* 需要换人的一方 */
    b->need_switch_me = 0;
    b->need_switch_foe = 0;
    BattleMon *mm = battle_active(b, 0);
    BattleMon *fm = battle_active(b, 1);
    if (mm && mm->cur_hp == 0) {
        b->need_switch_me = 1;
    }
    if (fm && fm->cur_hp == 0) {
        b->need_switch_foe = 1;
    }
    if (b->result == BATTLE_ONGOING) {
        if (b->need_switch_me && b->need_switch_foe) {
            return 3;
        }
        if (b->need_switch_me) {
            return 1;
        }
        if (b->need_switch_foe) {
            return 2;
        }
    }
    return 0;
}
