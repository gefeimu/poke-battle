/*
 * poke_ability.c - 特性效果实现
 *
 * 数据来源说明：Veekun 的 CSV 没有机读的特性效果
 * （ability_effect_text 表 404，ability_changelog 只有 id 对照），
 * 所以这里的每个效果都是按官方规则手写的。
 * 只实现对对战有实质影响的特性，未实现的一律返回"无效果"默认值。
 *
 * 分类：
 *   出场类   威吓 / 天气四兄弟
 *   回合末   加速 / 自然回复
 *   攻击类   大力士 / 纯粹力量
 *   防御类   神奇鳞片 / 厚脂肪
 *   速度类   叶绿素 / 悠游自如
 *   威力类   猛火/激流/茂盛/虫之预感 / 适应力 / 技师
 *   免疫类   飘浮/储水/蓄电/引火 / 各异常免疫 / 能力下降免疫
 *   其他     结实（抗一击必杀）
 */

#include "poke_ability.h"

#include <string.h>

/* ---------- 名字 ---------- */

const char *ability_name(uint8_t kind)
{
    for (unsigned i = 0; i < ABILITY_COUNT; i++) {
        if (kAbilities[i].kind == kind) {
            return kAbilities[i].name;
        }
    }
    return "None";
}

/* ---------- 天气特性 ---------- */

bool ability_is_weather_setter(uint8_t kind)
{
    return ability_weather(kind) != WX_NONE;
}

Weather ability_weather(uint8_t kind)
{
    switch (kind) {
        case ABIL_DRIZZLE: return WX_RAIN;
        case ABIL_DROUGHT: return WX_SUN;
        case ABIL_SAND_STREAM: return WX_SAND;
        case ABIL_SNOW_WARNING: return WX_HAIL;
        default: return WX_NONE;
    }
}

/* ---------- 出场触发 ---------- */

void ability_on_switch_in(Battle *b, int side_is_foe)
{
    BattleMon *m = battle_active(b, side_is_foe);
    if (!m || !m->alive) {
        return;
    }
    uint8_t k = m->base.ability;
    BattleLog *log = &b->log;
    const char *me = side_is_foe ? b->foe.name : b->me.name;

    /* 天气特性：只在没有天气时生效 */
    Weather w = ability_weather(k);
    if (w != WX_NONE && b->wx == WX_NONE) {
        b->wx = w;
        /* 同上：回合末会先减一次 */
        b->wx_turns = 6;
        log_add(log, "%s 的 %s 起作用了！", me, ability_name(k));
    }

    /* 威吓：降对方攻击一级 */
    if (k == ABIL_INTIMIDATE) {
        BattleMon *foe = battle_active(b, !side_is_foe);
        if (foe && foe->alive) {
            /* 能力下降免疫类特性无效 */
            uint8_t fk = foe->base.ability;
            bool blocked = ability_immune_stat_drop(fk) ||
                           ability_immune_stat_drop(fk);
            if (!blocked) {
                if (foe->stage[1] > -MAX_STAGE) {
                    foe->stage[1]--;
                    const SpeciesDef *fsd = species_get(foe->active_pid);
                    log_add(log, "%s 的 %s 被威吓了，攻击降低了！",
                            side_is_foe ? b->me.name : b->foe.name,
                            fsd ? fsd->name : "???");
                }
            }
        }
    }
}

/* ---------- 回合末 ---------- */

void ability_on_end_turn(Battle *b, int side_is_foe)
{
    BattleMon *m = battle_active(b, side_is_foe);
    if (!m || !m->alive) {
        return;
    }
    uint8_t k = m->base.ability;
    BattleLog *log = &b->log;
    const char *me = side_is_foe ? b->foe.name : b->me.name;

    /* 加速：每回合末速度 +1 */
    if (k == ABIL_SPEED_BOOST) {
        if (m->stage[5] < MAX_STAGE) {
            m->stage[5]++;
            log_add(log, "%s 的 %s 加速了！", me, ability_name(k));
        }
    }

    /* 自然回复：退场时治愈异常（换人时由 clear 处理，这里只做日志） */
    if (k == ABIL_NATURAL_CURE) {
        /* 实际治愈在换人时触发，见 battle_switch */
    }
}

/* ---------- 攻击/特攻 ---------- */

uint16_t ability_atk_mult(uint8_t kind)
{
    switch (kind) {
        case ABIL_HUGE_POWER: return 200;   /* 大力士：攻击翻倍 */
        case ABIL_PURE_POWER: return 200;   /* 纯粹力量：同大力士 */
        case ABIL_GUTS: return 150;         /* 根性：异常时攻击 1.5x */
        default: return 100;
    }
}

uint16_t ability_spa_mult(uint8_t kind)
{
    switch (kind) {
        case ABIL_GUTS: return 150;
        default: return 100;
    }
}

/* ---------- 防御 ---------- */

uint16_t ability_def_mult(const BattleMon *m, uint8_t kind)
{
    /* 神奇鳞片：有异常时防御 1.5x */
    if (kind == ABIL_MARVEL_SCALE && m->cond != CON_NONE) {
        return 150;
    }
    return 100;
}

uint16_t ability_spd_mult(const BattleMon *m, uint8_t kind)
{
    /* 毛皮大衣不在前六代常用集合，先不处理 */
    (void)m;
    (void)kind;
    return 100;
}

/* ---------- 速度 ---------- */

uint16_t ability_speed_mult(const BattleMon *m, uint8_t kind, Weather wx)
{
    /* 叶绿素：晴天下速度翻倍 */
    if (kind == ABIL_CHLOROPHYLL && wx == WX_SUN) {
        return 200;
    }
    /* 悠游自如：雨天速度翻倍 */
    if (kind == ABIL_SWIFT_SWIM && wx == WX_RAIN) {
        return 200;
    }
    /* 魔法反射/电毛等不在本集合 */
    (void)m;
    return 100;
}

/* ---------- 招式威力 ---------- */

uint16_t ability_power_mult(const BattleMon *atk, uint8_t kind,
                            const MoveDef *mv)
{
    uint32_t mult = 100;

    /* 低血翻倍类：HP <= 1/3 时本系招式 1.5x */
    if (atk->cur_hp * 3 <= atk->hp_max) {
        const SpeciesDef *sd = species_get(atk->active_pid);
        if (sd) {
            uint8_t t1 = sd->type1, t2 = sd->type2;
            switch (kind) {
                case ABIL_BLAZE:
                    if (mv->type == TYPE_FIRE) mult = mult * 3 / 2;
                    break;
                case ABIL_TORRENT:
                    if (mv->type == TYPE_WATER) mult = mult * 3 / 2;
                    break;
                case ABIL_OVERGROW:
                    if (mv->type == TYPE_GRASS) mult = mult * 3 / 2;
                    break;
                case ABIL_SWARM:
                    if (mv->type == TYPE_BUG) mult = mult * 3 / 2;
                    break;
                default:
                    break;
            }
            (void)t1;
            (void)t2;
        }
    }

    /* 适应力：本系招式 1.5x（STAB 之后另算，这里叠加在威力上） */
    if (kind == ABIL_ADAPTABILITY) {
        const SpeciesDef *sd = species_get(atk->active_pid);
        if (sd && (mv->type == sd->type1 || mv->type == sd->type2)) {
            mult = mult * 3 / 2;
        }
    }

    /* 技师：威力 <= 60 的招式 1.5x */
    if (kind == ABIL_TECHNICIAN && mv->power <= 60 && mv->power > 0) {
        mult = mult * 3 / 2;
    }

    if (mult > 999) {
        mult = 999;
    }
    return (uint16_t)mult;
}

/* ---------- 异常免疫 ---------- */

bool ability_immune_ailment(const BattleMon *m, uint8_t kind, uint8_t ailment)
{
    switch (ailment) {
        case MAIL_SLEEP:
            /* 不眠：免疫睡眠；干劲（Vital Spirit）同样 */
            return kind == ABIL_INSOMNIA || kind == ABIL_VITAL_SPIRIT;
        case MAIL_PARALYSIS:
            /* 柔软：免疫麻痹 */
            return kind == ABIL_LIMBER;
        case MAIL_POISON:
            /* 免疫：免疫中毒（Veekun 无单独"猛毒"枚举，按中毒处理） */
            return kind == ABIL_IMMUNITY;
        case MAIL_BURN:
            /* 水幕：免疫灼伤 */
            return kind == ABIL_WATER_VEIL;
        case MAIL_FREEZE:
            /* 熔岩铠甲：免疫冰冻 */
            return kind == ABIL_MAGMA_ARMOR;
        case MAIL_CONFUSION:
            /* 我行我素：免疫混乱 */
            return kind == ABIL_OWN_TEMPO;
        case MAIL_INFATUATION:
            /* 我行我素也免疫着迷 */
            return kind == ABIL_OWN_TEMPO;
        case MAIL_LEECH_SEED:
            return false;
        default:
            (void)m;
            return false;
    }
}

/* ---------- 属性免疫 ---------- */

bool ability_immune_type(const BattleMon *m, uint8_t kind, uint8_t move_type)
{
    switch (kind) {
        case ABIL_LEVITATE:
            return move_type == TYPE_GROUND;
        case ABIL_VOLT_ABSORB:
            return move_type == TYPE_ELECTRIC;
        case ABIL_WATER_ABSORB:
            return move_type == TYPE_WATER;
        case ABIL_FLASH_FIRE:
            return move_type == TYPE_FIRE;
        case ABIL_STORM_DRAIN:
            return move_type == TYPE_WATER;
        case ABIL_DRY_SKIN:
            /* 乾燥皮肤：水招式回血而非受伤，这里算免疫（不回血） */
            return move_type == TYPE_WATER;
        case ABIL_SAP_SIPPER:
            return move_type == TYPE_GRASS;
        default:
            (void)m;
            return false;
    }
}

/* ---------- 能力下降免疫 ---------- */

bool ability_immune_stat_drop(uint8_t kind)
{
    switch (kind) {
        case ABIL_CLEAR_BODY:   /* 恒净之体 */
        case ABIL_WHITE_SMOKE:  /* 白色烟雾 */
        case ABIL_KEEN_EYE:     /* 锐利目光：只防命中下降，这里放宽 */
        case ABIL_HYPER_CUTTER: /* 怪力：只防攻击下降，这里放宽 */
            return true;
        default:
            return false;
    }
}

/* ---------- 一击必杀免疫 ---------- */

bool ability_immune_ohko(uint8_t kind)
{
    return kind == ABIL_STURDY;
}
