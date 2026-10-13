/*
 * poke_ai.c - 对战 AI 实现（L1 启发式 + L2 2-ply minimax + L3 蒙特卡洛）
 *
 * 全文只用整数运算（ESP32-C3 无 FPU，soft-float 慢且占 Flash）。
 * 所有"期望伤害"以千分比（permil）表示：1000 = 正好打死。
 *
 * 与引擎保持模型一致的要点：
 *   - 伤害预测必须复刻 do_move 的威力修正链
 *     （特性倍率 x 道具倍率乘进临时 MoveDef），否则 AI 会
 *     系统性地高估/低估自己的输出；
 *   - 免疫判定直接用引擎的 ability_immune_type；
 *   - 命中率按引擎的 acc/eva 等级公式折算。
 */

#include "poke_ai.h"

#include "poke_ability.h"
#include "poke_item.h"
#include "poke_moves.h"
#include "poke_species.h"

#include <string.h>

/* ---------- 小工具 ---------- */

#define AI_MAX_CANDS 20

/* 终局分：远大于任何局面分的量级 */
#define AI_SCORE_WIN  1000000
#define AI_SCORE_LOSE -1000000

typedef struct {
    uint8_t is_switch;   /* 0 = 出招 1 = 换人 */
    uint8_t slot;
    uint16_t mega_pid;
    int32_t quick;       /* L1 启发式分 */
} AICand;

typedef struct {
    const AIConfig *cfg;
    int side;            /* AI 方：0 = me，1 = foe */
    long nodes;          /* 已消耗的模拟节点 */
    long node_cap;
    int samples;         /* 每个(我方候选,对手应手)的 RNG 取样次数 */
} AICtx;

/* 双方出战位（可能为 NULL） */
static const BattleMon *side_active(const Battle *b, int side)
{
    const BattleSide *s = side ? &b->foe : &b->me;
    if (s->active >= TEAM_SIZE) {
        return NULL;
    }
    return &s->team[s->active];
}

static uint16_t bst_of(const BattleMon *m)
{
    const SpeciesDef *sd = species_get(m->active_pid);
    if (!sd) {
        return 300;
    }
    return (uint16_t)(sd->hp + sd->atk + sd->def + sd->spa + sd->spd + sd->spe);
}

/* 实际速度（与引擎 effective_speed 同规则，引擎里那个是 static） */
static uint16_t eff_speed_of(const BattleMon *m, Weather wx)
{
    uint32_t s = calc_stat_staged(&m->base, 6, m->stage[5]);
    if (m->cond == CON_PARALYZE) {
        s /= 2;
    }
    s = s * ability_speed_mult(m, m->base.ability, wx) / 100;
    return (uint16_t)(s < 1 ? 1 : s);
}

/* ---------- 引擎已实现的效果白名单 ---------- */

/*
 * apply_move_effects 只实现了：伤害、7 种异常、能力变化、
 * 回复/吸取、四种天气。其余带 power==0 的招式（光墙/反射壁/
 * 吼叫/一角钻/守住/变身……）在引擎里是纯空转。
 * AI 必须知道这件事，否则会把废招当成好招。
 */
static bool ailment_implemented(uint8_t a)
{
    switch (a) {
        case MAIL_PARALYSIS:
        case MAIL_SLEEP:
        case MAIL_FREEZE:
        case MAIL_BURN:
        case MAIL_POISON:
        case MAIL_CONFUSION:
        case MAIL_LEECH_SEED:
            return true;
        default:
            return false;
    }
}

static bool move_implemented(const MoveDef *mv)
{
    if (mv->power > 0) {
        return true;
    }
    if (mv->ailment != MAIL_NONE && ailment_implemented(mv->ailment)) {
        return true;
    }
    if (mv->n_stat_changes > 0) {
        return true;
    }
    if (mv->heal > 0 || mv->drain > 0) {
        return true;
    }
    if (mv->category == MCAT_WHOLE_FIELD || mv->category == MCAT_FIELD) {
        if (mv->type == TYPE_WATER || mv->type == TYPE_FIRE ||
            mv->type == TYPE_ROCK || mv->type == TYPE_ICE) {
            return true; /* 天气招 */
        }
    }
    return false;
}

/* ---------- 伤害预测 ---------- */

/* 招式实际威力：复刻 do_move 的 特性倍率 x 道具倍率 */
static uint16_t move_power_eff(const BattleMon *atk, const MoveDef *mv)
{
    uint16_t mult = ability_power_mult(atk, atk->base.ability, mv);
    mult = (uint16_t)(mult * item_type_boost(atk->base.item, mv->type) / 100);
    if (mult == 100) {
        return mv->power;
    }
    uint32_t p = (uint32_t)mv->power * mult / 100;
    return (uint16_t)(p > 255 ? 255 : p);
}

/*
 * 引擎同款命中率折算（acc/eva 等级）。
 *
 * 条件必须与 poke_battle.c:533 逐字一致：
 *     if (mv->accuracy && mv->accuracy < 100) { ...掷骰... }
 * 也就是说 accuracy==0（必中）与 accuracy==100 都**不掷骰**。
 * 之前这里只把 0 当必中，导致 accuracy==100 的招（数据表里 116 个伤害招）
 * 会被闪避等级压到 75%，AI 系统性低估它们的期望伤害——
 * 而「带闪避的对手能否被一击打死」恰恰是最需要精算的场景。
 */
static unsigned acc_adjusted(const BattleMon *atk, const BattleMon *def,
                             const MoveDef *mv)
{
    if (mv->accuracy == 0 || mv->accuracy >= 100) {
        return 100; /* 必中，与引擎一致 */
    }
    int acc = mv->accuracy;
    int st = atk->stage_acc - def->stage_eva;
    if (st > 0) {
        acc = acc * (3 + st) / 3;
    } else if (st < 0) {
        acc = acc * 3 / (3 - st);
    }
    if (acc > 100) {
        acc = 100;
    }
    return (unsigned)acc;
}

/*
 * atk 对 def 的最大期望伤害（千分比，1000 = 打死）。
 * out_class / out_prio 可传 NULL：给出最强招的物理/特殊与优先度，
 * 供异常状态估值用（灼烧物理手、麻痹速度手）。
 */
static uint16_t best_dmg_permil_ex(const BattleMon *atk, const BattleMon *def,
                                   Weather wx, uint8_t *out_class,
                                   int8_t *out_prio)
{
    uint32_t best = 0;
    uint8_t best_class = DMG_STATUS;
    int8_t best_prio = 0;

    for (int i = 0; i < MOVES_PER_MON; i++) {
        uint16_t mid = atk->move[i];
        if (!mid || !atk->pp[i]) {
            continue;
        }
        /* 索引约定在下标 = id-1，务必走 mv_get 而不是裸下标 */
        const MoveDef *mv = mv_get(mid);
        if (!mv) {
            continue;
        }
        if (mv->power == 0) {
            continue;
        }
        if (type_effectiveness(mv->type, &def->base) == 0) {
            continue;
        }
        if (ability_immune_type(def, def->base.ability, mv->type)) {
            continue;
        }

        MoveDef eff = *mv;
        eff.power = move_power_eff(atk, mv);
        int did_crit;
        uint16_t dmg = calc_damage_ex(&atk->base, atk->cond, atk->stage,
                                      &def->base, def->cond, def->stage,
                                      &eff, wx, CALC_NO_ROLL, &did_crit);
        if (mv->max_hits > 1) { /* 多段按平均段数 */
            uint32_t avg = ((uint32_t)mv->min_hits + mv->max_hits) / 2;
            if (avg < 1) {
                avg = 1;
            }
            dmg = (uint16_t)((uint32_t)dmg * avg);
        }
        unsigned acc = acc_adjusted(atk, def, mv);
        uint32_t exp_dmg = (uint32_t)dmg * acc / 100;
        if (exp_dmg > best) {
            best = exp_dmg;
            best_class = mv->damage_class;
            best_prio = mv->priority;
        }
    }

    if (out_class) {
        *out_class = best_class;
    }
    if (out_prio) {
        *out_prio = best_prio;
    }
    if (!def->cur_hp) {
        return 1000;
    }
    uint32_t permil = best * 1000 / def->cur_hp;
    return (uint16_t)(permil > 3000 ? 3000 : permil);
}

static uint16_t best_dmg_permil(const BattleMon *atk, const BattleMon *def,
                                Weather wx)
{
    return best_dmg_permil_ex(atk, def, wx, NULL, NULL);
}

/* ---------- 局面静态评估 ---------- */

/*
 * 从 side 视角给局面打分（越大越好）。量级参考：
 *   一只满血 600 种族值的宝可梦 ≈ +84000
 *   一只濒死归零；被打死 ≈ -84000
 *   "我能稳吃对面 / 被对面稳吃" ≈ ±4000 + 克制差
 *   睡眠一回合 ≈ -15000
 */
int32_t ai_eval_position(const Battle *b, int side)
{
    int32_t total = 0;

    for (int s = 0; s < 2; s++) {
        const BattleSide *bs = s ? &b->foe : &b->me;
        int sign = (s == side) ? 1 : -1;
        int32_t f = 0;

        /* ---- 物资：存活宝可梦的种族值 x 血量比 ---- */
        for (int i = 0; i < TEAM_SIZE; i++) {
            const BattleMon *m = &bs->team[i];
            if (!m->alive || m->cur_hp == 0) {
                continue;
            }
            uint32_t hpm = m->hp_max ? (uint32_t)m->cur_hp * 100 / m->hp_max : 0;
            f += (int32_t)bst_of(m) * (40 + (int32_t)hpm);
        }

        /* 走 side_active() 而不是裸 team[active] 下标——它对
         * active >= TEAM_SIZE 返回 NULL。这里是公开 API
         * ai_eval_position，半初始化的 Battle 不该越界读。 */
        const BattleMon *act = side_active(b, side);
        const BattleMon *oact = side_active(b, !side);
        bool act_alive = act && act->alive && act->cur_hp > 0;
        bool oact_alive = oact && oact->alive && oact->cur_hp > 0;

        /* ---- 场上对抗 ---- */
        if (act_alive && oact_alive) {
            uint16_t threat = best_dmg_permil(act, oact, b->wx);
            uint16_t vuln = best_dmg_permil(oact, act, b->wx);
            f += ((int32_t)threat - (int32_t)vuln) * 50;
            if (threat >= 1000 && vuln < 1000) {
                f += 4000; /* 这回合我能先手/稳定吃它 */
            }
            if (vuln >= 1000 && threat < 1000) {
                f -= 4000; /* 被对面稳吃 */
            }
        }

        /* ---- 场上易失状态 ---- */
        if (act_alive) {
            int stage_sum = 0;
            for (int k = 1; k <= 5; k++) {
                stage_sum += act->stage[k];
            }
            f += stage_sum * 60;
            switch (act->cond) {
                case CON_SLEEP:    f -= 15000; break;
                case CON_FREEZE:   f -= 12000; break;
                case CON_PARALYZE: f -= 5000;  break;
                case CON_BURN:     f -= 3500;  break;
                case CON_POISON:   f -= 2500;  break;
                default: break;
            }
            if (act->confused) {
                f -= 1200;
            }
            if (act->leech_seeded) {
                f -= 1500;
            }
            if (!act->mega_done && ai_mon_can_mega(act)) {
                f += 3000; /* 还捏着一个 Mega，是资源 */
            }
        }

        total += sign * f;
    }
    return total;
}

/* ---------- L1：单候选启发式打分 ---------- */

/* 异常状态的价值（会按局面微调） */
static int32_t ailment_base_value(uint8_t a)
{
    switch (a) {
        case MAIL_SLEEP:      return 2500;
        case MAIL_PARALYSIS:  return 2000;
        case MAIL_FREEZE:     return 1800;
        case MAIL_BURN:       return 1300;
        case MAIL_POISON:     return 1200;
        case MAIL_LEECH_SEED: return 900;
        case MAIL_CONFUSION:  return 700;
        default:              return 0;
    }
}

static int32_t score_move(const Battle *b, const BattleMon *me,
                          const BattleMon *foe, uint8_t slot)
{
    /* 索引约定在下标 = id-1，务必走 mv_get 而不是裸下标 */
    const MoveDef *mv = mv_get(me->move[slot]);
    if (!mv) {
        return -20000; /* id 非法，视同废招 */
    }
    if (!move_implemented(mv)) {
        return -20000; /* 引擎里是空转的废招，永不选 */
    }

    uint8_t foe_class;
    int8_t foe_prio;
    uint16_t their_threat = best_dmg_permil_ex(foe, me, b->wx, &foe_class,
                                               &foe_prio);
    bool faster = eff_speed_of(me, b->wx) > eff_speed_of(foe, b->wx);
    /*
     * 先手判定必须用对手**所有合法招里的最高优先度**，而不是
     * best_dmg_permil_ex 顺带返回的「伤害最大招」的 priority。
     * 反例：对手同时有「伤害最高招 priority=0」和「抢先手招
     * priority=+1」，后者拿到的 foe_prio=0，AI 就误判自己先手，
     * 从而白拿 +3000「先手打死不会被打断」或免掉 -5000 重罚。
     * 引擎 who_first 比的是实际选中招的优先度。
     */
    int8_t foe_max_prio = foe_prio;
    for (int i = 0; i < MOVES_PER_MON; i++) {
        if (!foe->move[i] || !foe->pp[i]) {
            continue;
        }
        const MoveDef *fmv = mv_get(foe->move[i]);
        if (fmv && fmv->priority > foe_max_prio) {
            foe_max_prio = (int8_t)fmv->priority;
        }
    }
    /* 先制招即使速度慢也能先动 */
    bool i_first = faster || (mv->priority > foe_max_prio);
    int32_t sc = 0;

    if (mv->power > 0) {
        if (type_effectiveness(mv->type, &foe->base) == 0 ||
            ability_immune_type(foe, foe->base.ability, mv->type)) {
            return -100000; /* 绝对无效 */
        }

        MoveDef eff = *mv;
        eff.power = move_power_eff(me, mv);
        int did_crit;
        uint16_t dmg = calc_damage_ex(&me->base, me->cond, me->stage,
                                      &foe->base, foe->cond, foe->stage,
                                      &eff, b->wx, CALC_NO_ROLL, &did_crit);
        if (mv->max_hits > 1) {
            uint32_t avg = ((uint32_t)mv->min_hits + mv->max_hits) / 2;
            if (avg < 1) {
                avg = 1;
            }
            dmg = (uint16_t)((uint32_t)dmg * avg);
        }
        unsigned acc = acc_adjusted(me, foe, mv);
        uint32_t exp_dmg = (uint32_t)dmg * acc / 100;

        uint16_t permil = foe->cur_hp
                              ? (uint16_t)(exp_dmg * 1000 / foe->cur_hp)
                              : 1000;
        if (permil > 3000) {
            permil = 3000;
        }
        sc += (int32_t)permil * 3;

        if (exp_dmg >= foe->cur_hp && foe->cur_hp > 0) {
            sc += 4000; /* 期望伤害够打死 */
            if (i_first) {
                sc += 3000; /* 先手打死，不会被打断 */
            }
        } else if (!i_first && their_threat >= 1000) {
            sc -= 5000; /* 慢手且打不死，回合结束我没了 */
        }

        if (mv->flinch_chance > 0 && !i_first) {
            sc += 300; /* 慢手畏缩有机会打断对方 */
        }
        if (mv->drain > 0) {
            sc += 200;
        }

        /* 伤害附带的异常：按触发概率折价 */
        if (mv->ailment != MAIL_NONE && ailment_implemented(mv->ailment) &&
            mv->ailment_chance > 0 && foe->cond == CON_NONE) {
            int32_t v = ailment_base_value(mv->ailment);
            if (mv->ailment == MAIL_BURN) {
                /* 烧物攻手比烧特攻手有价值 */
                v = (foe_class == DMG_PHYSICAL) ? 1800 : 600;
            } else if (mv->ailment == MAIL_PARALYSIS) {
                v = (!faster || foe_prio > 0) ? 2200 : 900;
            }
            sc += (int32_t)v * (int32_t)mv->ailment_chance / 100 / 2;
        }

        /* 伤害附带的能力下降 */
        if (mv->n_stat_changes > 0 &&
            mv->category != MCAT_NET_GOOD_STATS &&
            mv->category != MCAT_DAMAGE_RAISE) {
            sc += 250;
        }
        return sc;
    }

    /* ---- 纯状态/变化招 ---- */
    if (mv->ailment != MAIL_NONE) {
        bool must = (mv->category == MCAT_AILMENT || mv->category == MCAT_SWAGGER);
        if (foe->cond != CON_NONE) {
            return -5000; /* 对方已有异常，别浪费回合 */
        }
        if (ability_immune_ailment(foe, foe->base.ability, mv->ailment)) {
            return -30000;
        }
        /* 电系麻痹招对地面无效（与引擎同规则） */
        if (mv->ailment == MAIL_PARALYSIS && mv->type == TYPE_ELECTRIC) {
            const SpeciesDef *gsd = species_get(foe->active_pid);
            if (gsd && (gsd->type1 == TYPE_GROUND || gsd->type2 == TYPE_GROUND)) {
                return -30000;
            }
        }
        /* 寄生种子对草系无效（引擎规则） */
        if (mv->ailment == MAIL_LEECH_SEED) {
            const SpeciesDef *fsd = species_get(foe->active_pid);
            if (fsd && (fsd->type1 == TYPE_GRASS || fsd->type2 == TYPE_GRASS)) {
                return -30000;
            }
        }
        unsigned chance = must ? 100u : mv->ailment_chance;
        int32_t v = ailment_base_value(mv->ailment);
        if (mv->ailment == MAIL_BURN) {
            v = (foe_class == DMG_PHYSICAL) ? 1800 : 600;
        } else if (mv->ailment == MAIL_PARALYSIS) {
            v = (!faster || foe_prio > 0) ? 2200 : 900;
        }
        sc += (int32_t)v * (int32_t)chance / 100;
        if (their_threat >= 1000) {
            sc += i_first ? 1000 : -4000; /* 先手封住血赚，慢手等于送死 */
        }
        return sc;
    }

    if (mv->heal > 0 && mv->category == MCAT_HEAL) {
        uint32_t missing = me->hp_max
                               ? (uint32_t)(me->hp_max - me->cur_hp) * 1000 / me->hp_max
                               : 0;
        sc += (int32_t)missing * 4; /* 空血 = +4000，满血 = 0 */
        if (their_threat >= 1000 && !i_first) {
            sc -= 3000; /* 慢手回复等于站着挨打 */
        }
        return sc;
    }

    if (mv->n_stat_changes > 0) {
        bool to_self = (mv->category == MCAT_NET_GOOD_STATS ||
                        mv->category == MCAT_DAMAGE_RAISE);
        if (to_self) {
            /* hp_max 兜底：同文件另两处都做了(m->hp_max ? ... : 0)，
             * 唯独这里裸除。当前 gen_cands 要求 cur_hp>0 才生成候选
             * 才打不到，但 int 除零在 ESP32-C3 上是异常。 */
            uint32_t hp_pct = me->hp_max ? (uint32_t)me->cur_hp * 100 / me->hp_max
                                         : 100;
            bool safe = their_threat < 350 && hp_pct > 55;
            sc += safe ? 1800 : 50;
            if (safe) {
                /* 已经叠得很高就别再来了 */
                for (unsigned i = 0; i < mv->n_stat_changes; i++) {
                    const StatChange *sc2 = &mv->stat_changes[i];
                    if (sc2->stat >= 1 && sc2->stat <= 6 &&
                        me->stage[sc2->stat - 1] + sc2->change > 3) {
                        sc -= 800;
                    }
                }
            } else if (their_threat >= 1000) {
                sc -= 4000; /* 快死了还强化，等于送 */
            }
        } else {
            if (ability_immune_stat_drop(foe->base.ability) ||
                item_immune_stat_drop(foe->base.item)) {
                sc -= 500;
            } else {
                sc += 350;
            }
        }
        return sc;
    }

    if ((mv->category == MCAT_WHOLE_FIELD || mv->category == MCAT_FIELD) &&
        (mv->type == TYPE_WATER || mv->type == TYPE_FIRE ||
         mv->type == TYPE_ROCK || mv->type == TYPE_ICE)) {
        sc += 300; /* 天气招：小幅正分，细节交给 L2 模拟 */
        return sc;
    }

    return -20000;
}

/*
 * 换人候选的 L1 分。量纲要和出招分可比（出招：造成 100% 期望
 * 伤害 ≈ +3000）。
 *
 * 换人的真实价值 = 每回合少受的伤 + 每回合多打的输出
 *                - 本回合白挨的那一下（换人消耗回合）
 * 早期版本有"满血 +2000""处于危险 +1500"这类常数项，等于无条件
 * 偏袒换人——每个替补都满血，于是 AI 每回合都想换人，测试里
 * 全线表现成"换人到 1 号位"。已改成纯边际计算。
 */
static int32_t score_switch_target(const Battle *b, const BattleMon *me,
                                   const BattleMon *bench,
                                   const BattleMon *foe)
{
    bool me_dead = (me->cur_hp == 0);
    uint16_t stay_vuln = me_dead ? 3000 : best_dmg_permil(foe, me, b->wx);
    uint16_t switch_vuln = best_dmg_permil(foe, bench, b->wx);
    uint16_t my_threat = best_dmg_permil(bench, foe, b->wx);
    uint16_t stay_threat = me_dead ? 0 : best_dmg_permil(me, foe, b->wx);

    int32_t sc = 0;
    sc += ((int32_t)stay_vuln - (int32_t)switch_vuln) * 2; /* 每回合少受伤 */
    sc += ((int32_t)my_threat - (int32_t)stay_threat) * 2; /* 每回合多输出 */
    sc -= (int32_t)switch_vuln;                            /* 本回合白挨一下 */
    return sc;
}

/* ---------- Mega ---------- */

bool ai_mon_can_mega(const BattleMon *m)
{
    if (m->mega_done || !mon_has_mega_stone(m)) {
        return false;
    }
    for (int i = 0; i < MEGA_COUNT; i++) {
        if (kMegaMap[i].base_species_id == m->active_pid) {
            return true;
        }
    }
    return false;
}

/* 该宝可梦可用的 Mega 形态数量 */
static int megas_of(const BattleMon *m, uint16_t *out, int max)
{
    int n = 0;
    if (!ai_mon_can_mega(m)) {
        return 0;
    }
    for (int i = 0; i < MEGA_COUNT && n < max; i++) {
        if (kMegaMap[i].base_species_id == m->active_pid) {
            out[n++] = kMegaMap[i].mega_pid;
        }
    }
    return n;
}

/* ---------- 候选生成 ---------- */

static void cand_sort_desc(AICand *a, int n)
{
    for (int i = 0; i < n - 1; i++) {
        for (int j = 0; j < n - 1 - i; j++) {
            if (a[j].quick < a[j + 1].quick) {
                AICand t = a[j];
                a[j] = a[j + 1];
                a[j + 1] = t;
            }
        }
    }
}

/*
 * 生成一方的全部候选并打 L1 分，按分降序返回。
 * 出战已濒死时只生成换人候选。
 * Mega 变体只挂在 L1 分最高的前 3 个招式上（控制分支数）。
 */
static int gen_cands(const Battle *b, int side, AICand *out, int max)
{
    const BattleSide *us = side ? &b->foe : &b->me;
    const BattleSide *them = side ? &b->me : &b->foe;
    const BattleMon *me = &us->team[us->active];
    const BattleMon *foe = &them->team[them->active];
    int n = 0;

    if (me->cur_hp > 0) {
        for (int i = 0; i < MOVES_PER_MON && n < max; i++) {
            if (!me->move[i] || !me->pp[i]) {
                continue;
            }
            out[n].is_switch = 0;
            out[n].slot = (uint8_t)i;
            out[n].mega_pid = 0;
            out[n].quick = score_move(b, me, foe, (uint8_t)i);
            n++;
        }

        /* Mega 变体：临时宝可梦副本套上 Mega 形态再打分。
         * 只挂在 L1 分最高的前 3 个招式上（喷火龙/超梦有双形态，
         * 4 招全挂会翻倍分支数）。此时 n 里只有出招候选（<=4）。 */
        uint16_t megas[2];
        int nm = megas_of(me, megas, 2);
        /*
         * 门槛是 n >= 1 而不是 n >= 3。n 是**当前有PP 的招式数**，
         * 残局里只剩2 招PP 时 n=2，原来的 n>=3 会让 Mega 候选一个
         * 都不生成——而 ai_eval_position 里「捏着 Mega 是资源 +3000」
         * 仍在生效，评估说有Mega 值得留，候选生成却不给用，
         * 两者自相矛盾。分支数上限由下面的 nt/ nm 双重 n < max
         * 约束保证，不需要用n 做闸门。
         */
        if (nm > 0 && n >= 1) {
            int top3[3];
            int nt = 0;
            while (nt < 3) {
                int best = -1;
                for (int j = 0; j < n; j++) {
                    int used = 0;
                    for (int k = 0; k < nt; k++) {
                        if (top3[k] == j) {
                            used = 1;
                        }
                    }
                    if (!used && (best < 0 || out[j].quick > out[best].quick)) {
                        best = j;
                    }
                }
                if (best < 0) {
                    break;
                }
                top3[nt++] = best;
            }
            for (int t = 0; t < nt && n < max; t++) {
                for (int mi = 0; mi < nm && n < max; mi++) {
                    BattleMon tmp = *me;
                    statsheet_set_mega(&tmp.base, megas[mi]);
                    tmp.active_pid = megas[mi];
                    out[n].is_switch = 0;
                    out[n].slot = out[top3[t]].slot;
                    out[n].mega_pid = megas[mi];
                    out[n].quick = score_move(b, &tmp, foe, out[top3[t]].slot);
                    n++;
                }
            }
        }
    }

    /* 换人候选 */
    for (int i = 0; i < TEAM_SIZE && n < max; i++) {
        if ((uint8_t)i == us->active) {
            continue;
        }
        const BattleMon *bm = &us->team[i];
        if (!bm->alive || bm->cur_hp == 0) {
            continue;
        }
        out[n].is_switch = 1;
        out[n].slot = (uint8_t)i;
        out[n].mega_pid = 0;
        out[n].quick = score_switch_target(b, me, bm, foe);
        n++;
    }

    cand_sort_desc(out, n);
    return n;
}

/* 把候选应用到战斗状态（Mega 先执行，不消耗回合） */
static void apply_cand(Battle *b, int side, const AICand *c)
{
    if (c->mega_pid) {
        battle_mega(b, side, c->mega_pid);
    }
    if (c->is_switch) {
        if (side) {
            b->foe_action = 0xFF;
            b->foe_switch = c->slot;
        } else {
            b->my_action = 0xFF;
            b->my_switch = c->slot;
        }
    } else {
        if (side) {
            b->foe_action = c->slot;
        } else {
            b->my_action = c->slot;
        }
    }
}

/* ---------- L2：2-ply 同时出手 minimax ---------- */

/* 终局折算成 AI 视角的分 */
static int32_t terminal_score(const Battle *b, int side)
{
    if (b->result == BATTLE_WIN) {
        return side == 0 ? AI_SCORE_WIN : AI_SCORE_LOSE;
    }
    if (b->result == BATTLE_LOSE) {
        return side == 0 ? AI_SCORE_LOSE : AI_SCORE_WIN;
    }
    if (b->result == BATTLE_DRAW) {
        return 0;
    }
    return ai_eval_position(b, side);
}

/*
 * 对排名前 my_cands 的候选各做一次 2-ply：
 * 应用我的候选 -> 枚举对手前 opp_replies 个应手 ->
 * 每个应手跑 samples 次 battle_step 取平均（吸收命中/暴击方差）
 * -> 按「对手威胁权重」加权平均。
 *
 * 为什么是加权期望而不是 min（曾经写成 min，实测把 AI 变笨）：
 *   min 语义是「防最坏情况」，但胜负分是 ±1000000 的量级，
 *   单个极端应手就能盖过所有局面分。结果是 AI 永远挑
 *   「不会被任何一种应手秒杀」的招，往往退化成保守换人，
 *   实测胜率从 73%掉到 48%。真正的目标是**最大化期望收益**：
 *   对手会理性选它的最优应手，但那不等于「我方要按最坏情况行动」。
 *
 * 权重用对手这招对我造成的期望伤害（越痛越可能真的被选），
 * 近似「对手最优响应」的分布，比等权平均更贴近真实博弈。
 *
 * 取样仍然必要：单个 battle_step 会把「我这招 miss 了」当成
 * 确定的结局，取样后才得到「这招的期望结果」。
 *
 * score[] 由调用方预先填成"未搜索"哨兵值（见 ai_choose），
 * 这里只覆盖真正搜索过的候选——这样预算耗尽时决策自动
 * 退化为 L1 最优（候选按 quick 降序，第一个就是）。
 */
static void search_2ply(AICtx *ctx, const Battle *b, const AICand *cands,
                        int n, int32_t *score)
{
    int top = n < ctx->cfg->my_cands ? n : ctx->cfg->my_cands;
    int replies = ctx->cfg->opp_replies;

    for (int i = 0; i < top; i++) {
        if (ctx->nodes > ctx->node_cap) {
            return; /* 预算耗尽，保留哨兵分 */
        }
        Battle c1 = *b;
        apply_cand(&c1, ctx->side, &cands[i]);

        AICand opp[AI_MAX_CANDS];
        int no = gen_cands(&c1, !ctx->side, opp, AI_MAX_CANDS);
        int k = no < replies ? no : replies;
        if (k <= 0) {
            continue; /* 无应手可枚举，留哨兵分 */
        }

        /*
         * 按对手威胁值加权平均。权重的直觉：对手更可能采用
         * 「能打我更多血」的应手，所以那些应手应该占更大权重。
         * 为保证无偏，用 「基础分 + 对该候选的威胁」——基础分让
         * 所有应手都有非零权重（避免 0 权除零），威胁值体现优先级。
         * 纯 0x 免疫的招威胁为 0，但仍保留基础权重参与平均，
         * 不会因分母为0 而跳过。
         */
        int32_t wsum = 0;
        int64_t acc = 0; /* 加权累加，用 64 位防溢出 */
        int evaluated = 0;

        for (int j = 0; j < k; j++) {
            /* 该对手应手对我造成的期望伤害（千分比），作权重 */
            const BattleMon *foe = side_active(&c1, !ctx->side);
            const BattleMon *me = side_active(&c1, ctx->side);
            uint16_t threat = 0;
            if (foe && me) {
                uint8_t fc;
                int8_t fprio = 0;
                threat = best_dmg_permil_ex(foe, me, c1.wx, &fc, &fprio);
            }

            int32_t vsum = 0;
            int ns = 0;
            for (int s = 0; s < ctx->samples; s++) {
                if (ctx->nodes > ctx->node_cap) {
                    break;
                }
                Battle c2 = c1;
                apply_cand(&c2, !ctx->side, &opp[j]);
                ctx->nodes++;
                battle_step(&c2);
                vsum += terminal_score(&c2, ctx->side);
                ns++;
            }
            if (ns > 0) {
                /* 基础权重 1000 + 威胁（0..3000），保证恒为正 */
                int32_t w = 1000 + (int32_t)threat;
                acc += (int64_t)w * (int64_t)(vsum / ns);
                wsum += w;
                evaluated++;
            }
        }

        if (evaluated > 0 && wsum > 0) {
            score[i] = (int32_t)(acc / wsum);
        }
    }
}

/* ---------- L3：蒙特卡洛 ---------- */

/*
 * 蒙特卡洛里的简化选招器（比 gen_cands 便宜一个量级）：
 * 只比较期望伤害；没有伤害招就随便用一个；
 * 10% 概率随机换招增加多样性；
 * 压力大（会被秒）且替补明显更能抗时，30% 概率换人。
 */
static void pick_playout_action(AICtx *ctx, Battle *sim, int side,
                                uint8_t *action, uint8_t *sw, uint16_t *mega)
{
    (void)ctx;
    *action = 0;
    *sw = 0;
    *mega = 0;

    const BattleSide *us = side ? &sim->foe : &sim->me;
    const BattleSide *them = side ? &sim->me : &sim->foe;
    const BattleMon *me = &us->team[us->active];
    const BattleMon *foe = &them->team[them->active];

    /* 濒死：只能换人 */
    if (me->cur_hp == 0) {
        for (int i = 0; i < TEAM_SIZE; i++) {
            if ((uint8_t)i == us->active) {
                continue;
            }
            if (us->team[i].alive && us->team[i].cur_hp > 0) {
                *action = 0xFF;
                *sw = (uint8_t)i;
                return;
            }
        }
        *action = 0xFF;
        *sw = us->active; /* 无药可救，交给引擎 */
        return;
    }

    /* 期望伤害最大的招 */
    int best = -1;
    uint32_t best_dmg = 0;
    int legal[4];
    int nlegal = 0;
    for (int i = 0; i < MOVES_PER_MON; i++) {
        if (!me->move[i] || !me->pp[i]) {
            continue;
        }
        legal[nlegal++] = i;
        const MoveDef *mv = mv_get(me->move[i]);
        if (!mv) {
            continue;
        }
        if (mv->power == 0 ||
            type_effectiveness(mv->type, &foe->base) == 0 ||
            ability_immune_type(foe, foe->base.ability, mv->type)) {
            continue;
        }
        MoveDef eff = *mv;
        eff.power = move_power_eff(me, mv);
        int did_crit;
        uint16_t dmg = calc_damage_ex(&me->base, me->cond, me->stage,
                                      &foe->base, foe->cond, foe->stage,
                                      &eff, sim->wx, CALC_NO_ROLL, &did_crit);
        if (mv->max_hits > 1) {
            uint32_t avg = ((uint32_t)mv->min_hits + mv->max_hits) / 2;
            dmg = (uint16_t)((uint32_t)dmg * (avg < 1 ? 1 : avg));
        }
        uint32_t exp = (uint32_t)dmg * acc_adjusted(me, foe, mv) / 100;
        if ((int)exp > (int)best_dmg || best < 0) {
            best_dmg = exp;
            best = i;
        }
    }

    /* 10% 随机换招 */
    if (nlegal > 1 && (battle_rng() % 100) < 10) {
        *action = (uint8_t)legal[battle_rng() % (unsigned)nlegal];
        return;
    }
    if (best >= 0) {
        *action = (uint8_t)best;
        return;
    }

    /* 没有可用伤害招：随机用一个合法的（状态招） */
    if (nlegal > 0) {
        *action = (uint8_t)legal[battle_rng() % (unsigned)nlegal];
        return;
    }

    /* 什么都没得用：试着换人 */
    for (int i = 0; i < TEAM_SIZE; i++) {
        if ((uint8_t)i == us->active) {
            continue;
        }
        if (us->team[i].alive && us->team[i].cur_hp > 0) {
            *action = 0xFF;
            *sw = (uint8_t)i;
            return;
        }
    }
    *action = 0; /* 等死，引擎会记"什么都做不了" */
}

/*
 * 从"我方候选已应用"的局面开始对打。
 * sim->{my,foe}_action 里我方那一侧已由调用方填好（首回合沿用候选），
 * 之后双方都用简化启发式。
 */
static int32_t mc_playout(AICtx *ctx, Battle *sim)
{
    int side = ctx->side;
    uint16_t turn_start = sim->turn;
    bool first = true;

    while (sim->result == BATTLE_ONGOING &&
           sim->turn < turn_start + 20 &&
           ctx->nodes < ctx->node_cap) {
        /* 上一回合遗留的强制换人。两侧都要处理，但一旦某方
         * 已经没有可上场的替补，这一局就分出胜负了——必须直接退出，
         * 否则 battle_switch 失败 + battle_step 空转会让本循环
         * 一直转到 node_cap（实测单场对局会卡到几十秒）。 */
        if (!side_active(sim, side) || !side_active(sim, !side)) {
            break; /* active 越界，局面已不可用*/
        }
        if (side_active(sim, side)->cur_hp == 0) {
            uint8_t a, sw;
            uint16_t mg;
            pick_playout_action(ctx, sim, side, &a, &sw, &mg);
            if (a != 0xFF) {
                break; /* 没得换，这一方已经输了 */
            }
            if (battle_switch(sim, side, sw) != 0) {
                break;
            }
        }
        if (side_active(sim, !side)->cur_hp == 0) {
            uint8_t a, sw;
            uint16_t mg;
            pick_playout_action(ctx, sim, !side, &a, &sw, &mg);
            if (a != 0xFF) {
                break;
            }
            if (battle_switch(sim, !side, sw) != 0) {
                break;
            }
        }

        if (!first) {
            uint8_t a, sw;
            uint16_t mg;
            pick_playout_action(ctx, sim, side, &a, &sw, &mg);
            if (side) {
                sim->foe_action = a;
                sim->foe_switch = sw;
            } else {
                sim->my_action = a;
                sim->my_switch = sw;
            }
        }
        {
            uint8_t a, sw;
            uint16_t mg;
            pick_playout_action(ctx, sim, !side, &a, &sw, &mg);
            if (!side) {
                sim->foe_action = a;
                sim->foe_switch = sw;
            } else {
                sim->my_action = a;
                sim->my_switch = sw;
            }
        }

        ctx->nodes++;
        battle_step(sim);
        first = false;
    }

    return terminal_score(sim, side);
}

/* ---------- 对外：决策 ---------- */

void ai_config_default(AIConfig *cfg, uint8_t level)
{
    memset(cfg, 0, sizeof(*cfg));
    cfg->level = level;
    switch (level) {
        case AI_ROOKIE:
            cfg->max_nodes = 0;
            cfg->my_cands = 0;
            cfg->opp_replies = 0;
            cfg->mc_playouts = 0;
            break;
        case AI_SMART:
        default:
            cfg->max_nodes = 220; /* L2 6x4x2=48 + MC 6x20=120，留余量 */
            cfg->my_cands = 6;
            cfg->opp_replies = 4;
            cfg->mc_playouts = 6;
            break;
        case AI_EXPERT:
            cfg->max_nodes = 700; /* L2 8x6x3=144 + MC 16x20=320，留余量 */
            cfg->my_cands = 8;
            cfg->opp_replies = 6;
            cfg->mc_playouts = 16;
            break;
    }
}

const char *ai_level_name(uint8_t level)
{
    switch (level) {
        case AI_ROOKIE: return "新手";
        case AI_SMART:  return "老练";
        case AI_EXPERT: return "冠军";
        default:        return "???";
    }
}

/* 动作是否合法 */
static bool action_legal(const Battle *b, int side, const AIAction *a)
{
    const BattleSide *us = side ? &b->foe : &b->me;
    if (us->active >= TEAM_SIZE) {
        return false;
    }
    const BattleMon *me = &us->team[us->active];
    if (a->mega_pid) {
        if (!ai_mon_can_mega(me)) {
            return false;
        }
        if (mega_to_base(a->mega_pid) != me->active_pid) {
            return false;
        }
    }
    if (a->is_switch) {
        if (a->slot >= TEAM_SIZE || (uint8_t)a->slot == us->active) {
            return false;
        }
        const BattleMon *t = &us->team[a->slot];
        return t->alive && t->cur_hp > 0;
    }
    if (a->slot >= MOVES_PER_MON) {
        return false;
    }
    return me->move[a->slot] && me->pp[a->slot] > 0;
}

void ai_choose(const Battle *b, int ai_is_foe, const AIConfig *cfg,
               AIAction *out)
{
    AICtx ctx;
    ctx.cfg = cfg;
    ctx.side = ai_is_foe ? 1 : 0;
    ctx.nodes = 0;
    ctx.node_cap = cfg ? cfg->max_nodes : 0;
    /* 每个应手的 RNG 取样次数：档位越高取样越多，期望越稳 */
    ctx.samples = (cfg && cfg->level == AI_EXPERT) ? 3 : 2;

    /* 默认输出：随便打第一招（调用方忘记传 cfg 时的兜底） */
    out->is_switch = 0;
    out->slot = 0;
    out->mega_pid = 0;

    /* 搜索期间静音 + 保存随机流，避免消耗真实对局的熵 */
    bool log_on = battle_log_enabled();
    uint32_t saved_rng = *battle_rng_state();
    battle_log_enable(false);

    AICand cands[AI_MAX_CANDS];
    int n = gen_cands(b, ctx.side, cands, AI_MAX_CANDS);

    if (n == 0) {
        /* 极端情况：没招也没替补。交给引擎记日志 */
        battle_log_enable(log_on);
        *battle_rng_state() = saved_rng;
        return;
    }

    const BattleSide *us = ctx.side ? &b->foe : &b->me;
    bool forced_switch = us->team[us->active].cur_hp == 0;
    bool use_search = (cfg && cfg->level != AI_ROOKIE && cfg->my_cands > 0 &&
                       !forced_switch);

    int pick = 0;

    if (!use_search) {
        /* ---- L1 only（ROOKIE / 强制换人）---- */
        if (cfg && cfg->level == AI_ROOKIE && n > 1 &&
            (battle_rng() % 100) < 15) {
            /* 新手偶尔失手：从前三名里随机 */
            int nt = n < 3 ? n : 3;
            pick = (int)(battle_rng() % (unsigned)nt);
        } else {
            pick = 0;
        }
    } else {
        /* ---- L2 ---- */
        /*
         * 哨兵分：比任何真实评估分都低，且保持候选的 L1 顺序，
         * 这样预算耗尽 / 无候选可搜索时自动退化为 L1 最优。
         */
        int32_t search[AI_MAX_CANDS];
        for (int i = 0; i < n; i++) {
            search[i] = AI_SCORE_LOSE - 1 - (int32_t)i;
        }
        search_2ply(&ctx, b, cands, n, search);

        /*
         * 融合 L1 与搜索分。
         *
         * 关键：两者量纲差两个数量级（eval 是 1e5~1e6 的物资项，
         * L1 是 1e4 量级的战术项），直接用搜索分覆盖 L1 会让
         * 只看物资的 eval 主导决策、把 L1 里「能打死人」「会被秒」
         * 这类关键判断冲掉——实测胜率会从 73% 掉到 47%。
         *
         * 所以做法是：先把搜索分归一化到 [-1,1]（除以终局分量级），
         * 再乘一个远小于 L1 分跨度的权重加到 L1 上。这样搜索是
         * 「修正项」而非「替代项」——它能打破 L1 内部的平局、纠正
         * L1 看漏的战术，但不会推翻 L1 已经确认的大方向。
         */
        int32_t score[AI_MAX_CANDS];
        for (int i = 0; i < n; i++) {
            int32_t s = search[i];
            if (s <= AI_SCORE_LOSE) {
                /* 没被搜索到（预算耗尽/无法枚举），直接用 L1 */
                score[i] = cands[i].quick * 100;
                continue;
            }
            /* search 归一化：AI_SCORE_WIN/LOSE 约 1e6 */
            int32_t norm = (s * 100) / AI_SCORE_WIN;
            /* 修正权重：L1 候选间的典型分差在 1e4 量级，
             * 这里给 ±6000 的调整空间——足以翻盘但不会喧宾夺主 */
            score[i] = cands[i].quick * 100 + norm * 200;
        }

        /* ---- L3：对前 3 名做蒙特卡洛 ---- */
        int nmc = cfg->mc_playouts;
        if (nmc > 0 && ctx.nodes < ctx.node_cap) {
            int top3[3];
            int nt = 0;
            for (int t = 0; t < 3; t++) {
                int best = -1;
                for (int i = 0; i < n; i++) {
                    int used = 0;
                    for (int k = 0; k < nt; k++) {
                        if (top3[k] == i) {
                            used = 1;
                        }
                    }
                    /* 用 search[] 判定「是否真的被搜索过」——
                     * score[] 已经和 L1 融合过，不再含哨兵值 */
                    if (used || search[i] <= AI_SCORE_LOSE) {
                        continue;
                    }
                    if (best < 0 || score[i] > score[best]) {
                        best = i;
                    }
                }
                if (best < 0) {
                    break;
                }
                top3[nt++] = best;
            }
            for (int t = 0; t < nt; t++) {
                int32_t acc = 0;
                int done = 0;
                for (int p = 0; p < nmc; p++) {
                    if (ctx.nodes >= ctx.node_cap) {
                        break;
                    }
                    Battle sim = *b;
                    apply_cand(&sim, ctx.side, &cands[top3[t]]);
                    acc += mc_playout(&ctx, &sim);
                    done++;
                }
                if (done > 0) {
                    /* 蒙特卡洛结论同样归一化后叠加。
                     * mc_playout 返回的 terminal_score 量级 1e5~1e6，
                     * 直接累加会盖过 L1（实测导致胜率 73% -> 47%）。
                     * 归一到 [-1,1] 再乘权重，保持「搜索是修正项」的定位。 */
                    int32_t avg = acc / done;
                    int32_t norm = (avg * 100) / AI_SCORE_WIN;
                    score[top3[t]] += norm * (cfg->level == AI_EXPERT ? 160 : 90);
                }
            }
        }

        /* 选最高分（并列时取 L1 分高的） */
        for (int i = 1; i < n; i++) {
            if (score[i] > score[pick] ||
                (score[i] == score[pick] &&
                 cands[i].quick > cands[pick].quick)) {
                pick = i;
            }
        }
    }

    /*
 * 合法性兜底。
 *
 * 必须从 pick 起扫：pick 是 L2/L3 搜索得出的最优候选，cands 已按
 * L1 分降序排列，所以「从 0 开始找第一个合法的」永远等于 cands[0]，
 * 会把搜索结果整���丢掉（L2/L3 白跑，AI 退化成纯 L1）。
 * 若 pick 本身不合法（理论上不会，gen_cands 已过滤），再回退到全量扫描。
 */
int chosen = -1;
for (int i = pick; i < n; i++) {
    AIAction t;
    t.is_switch = cands[i].is_switch;
    t.slot = cands[i].slot;
    t.mega_pid = cands[i].mega_pid;
    if (action_legal(b, ctx.side, &t)) {
        chosen = i;
        break;
    }
}
if (chosen < 0) {
        for (int i = 0; i < n; i++) {
            AIAction t;
            t.is_switch = cands[i].is_switch;
            t.slot = cands[i].slot;
            t.mega_pid = cands[i].mega_pid;
            if (action_legal(b, ctx.side, &t)) {
                chosen = i;
                break;
            }
        }
    }
    if (chosen < 0) {
        chosen = 0; /* 全都不合法：交给调用方，走引擎的兜底 */
    }

    out->is_switch = cands[chosen].is_switch;
    out->slot = cands[chosen].slot;
    out->mega_pid = cands[chosen].mega_pid;

    battle_log_enable(log_on);
    *battle_rng_state() = saved_rng;
}

void ai_apply(Battle *b, int side_is_foe, const AIAction *a)
{
    if (a->mega_pid) {
        battle_mega(b, side_is_foe, a->mega_pid);
    }
    if (a->is_switch) {
        if (side_is_foe) {
            b->foe_action = 0xFF;
            b->foe_switch = a->slot;
        } else {
            b->my_action = 0xFF;
            b->my_switch = a->slot;
        }
    } else {
        if (side_is_foe) {
            b->foe_action = a->slot;
        } else {
            b->my_action = a->slot;
        }
    }
}
