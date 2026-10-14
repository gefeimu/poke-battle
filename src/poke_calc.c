/*
 * poke_calc.c - 能力值与伤害公式实现（第六代规则）
 *
 * 与 Gen III 的三处实质差异（已逐条标注）：
 *   1) 暴击倍率 2.0 -> 1.5
 *   2) 随机因子 217..255/255 -> 85..100/100
 *   3) 沙暴/冰雹不再直接加减伤害（沙暴只把岩石系特防 *1.5，
 *      那部分属于能力值修正，不在伤害链里）
 *
 * 公式参考 Smogon 的 Gen VI 伤害计算说明。
 * 全部用 32 位整数重写，规避原版 8 位有符号数的溢出陷阱。
 */

#include "poke_calc.h"

/* ---------- 内部工具 ---------- */

/* 除法取整，避免浮点 */
static inline uint32_t fdiv(uint32_t a, uint32_t b) { return b ? a / b : 0; }

/* 定点乘：a * num / den，四舍五入到最近（.5 向上） */
static inline uint32_t mul_ratio(uint32_t a, uint32_t num, uint32_t den)
{
    if (den == 0) {
        return a;
    }
    return (a * num * 2 + den) / (den * 2);
}

/* 性格修正：目标 stat 上升 +10%，下降 -10%。
 * 上升用 x*11/10，下降用 x*9/10。 */
static uint16_t apply_nature(uint16_t v, uint8_t nature, uint8_t stat_idx)
{
    const NatureDef *n = &kNatures[nature];
    if (n->increased_stat == stat_idx) {
        return (uint16_t)(v * 11 / 10);
    }
    if (n->decreased_stat == stat_idx) {
        return (uint16_t)(v * 9 / 10);
    }
    return v;
}

/* 通用「等级 -> 倍率」，官方定义（base=2 为能力值，base=3 为命中/闪避）：
 *   stage > 0 : (base + stage) / base
 *   stage < 0 : base / (base + |stage|)
 * 净等级先钳到 ±MAX_STAGE（官方规则：命中/闪避的**净值**也是 ±6 封顶）。
 * 取整用截断 —— 官方对 100 点给出的就是 66/50/40/33/28/25，不是四舍五入。 */
uint16_t calc_stage_ratio(uint16_t v, int8_t stage, uint8_t base)
{
    if (stage == 0 || base == 0) {
        return v;
    }
    int idx = stage;
    if (idx < -MAX_STAGE) idx = -MAX_STAGE;
    if (idx > MAX_STAGE) idx = MAX_STAGE;

    uint32_t num;
    uint32_t den;
    if (idx > 0) {
        num = (uint32_t)base + (uint32_t)idx;
        den = base;
    } else {
        num = base;
        den = (uint32_t)base + (uint32_t)(-idx);
    }
    uint16_t r = (uint16_t)fdiv((uint32_t)v * num, den);
    return r;
}

/*
 * 能力等级修正（Gen III 起未变，官方倍率）：
 *
 *   +1..+6 ：(2 + n) / 2   -> 1.5  2.0  2.5  3.0  3.5  4.0
 *   -1..-6 ：2 / (2 + n)   -> 0.66 0.5  0.4  0.33 0.28 0.25
 *
 * 这里**曾经**是一张手写的 NUM[13]/DEN[13] 查表，13 档里错 11 档：
 * 所有负档塌成 1.0x 或 0.67x（-1/-3/-5 降能力完全无效，威吓/叫声/瞪眼
 * 成了摆设），正的 +2..+6 又严重放大（+6 算成 5 倍）。而 test_calc 对
 * 能力等级一条断言都没有，所以「全绿却是错的」。
 *
 * 现在走 calc_stage_ratio(base=2)，命中/闪避等级走同一函数（base=3），
 * 不再有第二套内联实现。
 */
static uint16_t apply_stage(uint16_t v, int8_t stage)
{
    uint16_t r = calc_stage_ratio(v, stage, 2);
    return r ? r : 1;
}

/* 从种族值表取基础值 */
static uint16_t base_stat_of(uint16_t species_id, uint8_t stat_idx)
{
    const SpeciesDef *sd = species_get(species_id);
    if (!sd) {
        return 50;
    }
    switch (stat_idx) {
        case 1: return sd->hp;
        case 2: return sd->atk;
        case 3: return sd->def;
        case 4: return sd->spa;
        case 5: return sd->spd;
        case 6: return sd->spe;
        default: return 50;
    }
}

/* ---------- 能力值 ---------- */

uint16_t calc_hp(const Statsheet *s)
{
    uint32_t base = base_stat_of(s->species_id, 1);
    uint32_t iv = s->ey.iv[0];
    uint32_t ev = s->ey.ev[0];
    uint32_t lv = s->level;

    /*
     * Gen III+ 官方式，**HP 不受性格影响**：
     *   HP = floor((2*Base + IV + floor(EV/4)) * Level / 100) + Level + 10
     *
     * 这里曾经写成 `(base + iv/4) * 2 * lv / 100 + lv + 10 + ev / 4`：
     *   1) (base + iv/4)*2 = 2*base + floor(iv/2)，**IV 的贡献被砍半**
     *   2) EV/4 加在 ×Level/100 **之外**，于是低等级时努力值被严重高估
     * 妙蛙种子 Lv50/31IV/0EV 的 HP 因此从正确的 120 变成 112；
     * 单项 252 努力值时差距更大。与能力值那处是同一个 bug。
     */
    uint32_t h = (2 * base + iv + ev / 4) * lv / 100 + lv + 10;
    if (h > 999) h = 999;
    if (h < 1) h = 1;
    return (uint16_t)h;
}

uint16_t calc_hp_max(const Statsheet *s)
{
    return calc_hp(s);
}

uint16_t calc_stat(const Statsheet *s, uint8_t stat_idx)
{
    if (stat_idx > 6) {
        return 0;
    }
    if (stat_idx == 1) {
        return calc_hp(s);
    }

    uint32_t base = base_stat_of(s->species_id, stat_idx);
    uint32_t iv = s->ey.iv[stat_idx - 1];
    uint32_t ev = s->ey.ev[stat_idx - 1];
    uint32_t lv = s->level;

    /*
     * Gen III+ 官方式：
     *   其它 = floor((2*Base + IV + floor(EV/4)) * Level / 100) + 5
     * 之后才乘性格修正（这也是官方顺序：性格在最后一步）。
     *
     * 曾经写成 `((base + iv/4) * 2 * lv) / 100 + 5 + ev / 4`，
     * 两处都错（IV 砍半、EV 乘在等级缩放之外）。详见 calc_hp。
     */
    uint32_t v = (2 * base + iv + ev / 4) * lv / 100 + 5;

    /* 性格 */
    v = apply_nature((uint16_t)v, s->nature, stat_idx);

    /*
     * 携带道具的能力值加成（粗骨头/电气球等固定翻倍类）。
     * **只查数据表**（item_get），不依赖 poke_item.c 的 item_stat_boost ——
     * calc 层保持纯计算、可独立单测。以前这个加成只有 item_stat_boost
     * 一处实现、无人调用，等于道具白带；现在统一由这里承担。
     */
    const ItemDef *it = item_get(s->item);
    if (it && it->boost_stat == stat_idx && it->stat_amount > 0) {
        v = v * (uint32_t)(100 + it->stat_amount) / 100;
    }

    if (v > 999) v = 999;
    if (v < 1) v = 1;
    return (uint16_t)v;
}

/* 带能力等级的能力值。stage 传 NULL 视为 0。
 * HP（stat_idx==1）不吃能力等级，直接走 calc_stat。 */
uint16_t calc_stat_staged(const Statsheet *s, uint8_t stat_idx, int8_t stage)
{
    uint16_t v = calc_stat(s, stat_idx);
    if (stat_idx == 1) {
        return v;
    }
    return apply_stage(v, stage);
}

/* ---------- 属性克制 ---------- */

uint8_t type_effectiveness(uint8_t move_type, const Statsheet *def)
{
    const SpeciesDef *sd = species_get(def->species_id);
    if (!sd || move_type >= TYPE_COUNT) {
        return 10;
    }
    uint8_t t1 = sd->type1;
    uint8_t t2 = sd->type2;
    /* 单属性宝可梦的 type2 在表里等于 type1，下面 t2 != t1 的判断
     * 天然把重复属性只算一次，不需要额外分支。 */
    uint8_t e1 = kTypeEffect[move_type][t1];
    uint8_t e2 = (t2 != t1) ? kTypeEffect[move_type][t2] : 10;
    return (uint8_t)((e1 * e2) / 10);
}

/* ---------- 伤害 ---------- */

/*
 * Gen VI 伤害链，顺序不能乱（整数截断会影响 1~2 点伤害）：
 *
 *   base -> 天气 -> 暴击 -> 随机 -> STAB -> 克制 -> 灼伤
 *
 * 与 Gen III 的差异：
 *   - 暴击 1.5 而非 2.0
 *   - 随机 85..100/100 而非 217..255/255
 *   - 沙暴/冰雹不进伤害链
 *   - 暴击时忽略**防守方**的能力等级与攻方灼伤（Gen III 同）
 */
uint16_t calc_damage_ex(const Statsheet *atk_s, Condition atk_cond,
                        const int8_t *atk_stage,
                        const Statsheet *def_s, Condition def_cond,
                        const int8_t *def_stage,
                        const MoveDef *mv, Weather wx, int16_t crit_percent,
                        int *did_crit)
{
    /* def_cond 目前不进公式：防守方的灼伤/中毒/麻痹都不降低防御，
     * 麻痹只降速度（在速度判定处处理）。参数先留着，
     * 以后加"冰冻怕火招"之类的特性时不用改签名。 */
    (void)def_cond;

    if (did_crit) {
        *did_crit = 0;
    }
    if (!mv || mv->power == 0) {
        return 0;
    }
    if (!atk_s || !def_s) {
        return 0;
    }

    /* Gen IV 起物理/特殊按招式分，不再按属性分。
     * MoveDef.damage_class 就是判据：DMG_PHYSICAL -> atk/def，
     * 其余（DMG_SPECIAL）-> spa/spd。状态招式已在上面 power==0 挡掉。 */
    bool physical = (mv->damage_class == DMG_PHYSICAL);
    uint8_t atk_stat = physical ? 2 : 4;
    uint8_t def_stat = physical ? 3 : 5;

    int8_t a_stage = atk_stage ? atk_stage[atk_stat - 1] : 0;
    int8_t d_stage = def_stage ? def_stage[def_stat - 1] : 0;

    /* CALC_NO_ROLL：不暴击且随机因子取 100（确定值）。
     * AI 预估伤害必须用这个——否则每次预估都被 85~100 的
     * 随机因子污染，同一局面两次算出不同的数，
     * 决策不可重放，评估函数也不再满足对称性。 */
    bool no_roll = (crit_percent == CALC_NO_ROLL);

    uint32_t A = calc_stat_staged(atk_s, atk_stat, a_stage);
    uint32_t D = calc_stat_staged(def_s, def_stat, d_stage);

    /* 是否暴击。crit_percent<0 表示调用方强制暴击（单测用）。 */
    bool crit = no_roll ? false : (crit_percent < 0);
    if (did_crit) {
        *did_crit = crit ? 1 : 0;
    }

    /* 暴击忽略攻方灼伤与防守方能力等级 */
    if (crit && d_stage != 0) {
        D = calc_stat(def_s, def_stat);
    }
    if (atk_cond == CON_BURN && physical && !crit) {
        A = A / 2;
    }
    if (D == 0) {
        D = 1;
    }

    uint32_t lv = atk_s->level;
    uint32_t P = mv->power;

    /* BaseDamage = floor(floor(floor(2*L/5+2) * A * P / D) / 50) + 2 */
    uint32_t d = fdiv(fdiv((2 * lv / 5 + 2) * A * P, D), 50) + 2;

    /* 天气：只影响火/水。沙暴与冰雹不减伤害。 */
    switch (wx) {
        case WX_RAIN:
            if (mv->type == TYPE_WATER) d = d * 3 / 2;
            else if (mv->type == TYPE_FIRE) d = d / 2;
            break;
        case WX_SUN:
            if (mv->type == TYPE_FIRE) d = d * 3 / 2;
            else if (mv->type == TYPE_WATER) d = d / 2;
            break;
        case WX_SAND:
        case WX_HAIL:
        case WX_NONE:
        default:
            break;
    }

    /* 暴击 1.5（Gen VI；Gen III 是 2.0） */
    if (crit) {
        d = d * 3 / 2;
    }

    /* 随机因子 85..100 / 100（Gen VI；Gen III 是 217..255/255） */
    uint32_t r = no_roll ? 100u : calc_rand_85_100();
    d = mul_ratio(d, r, 100);

    /* STAB */
    const SpeciesDef *asd = species_get(atk_s->species_id);
    if (asd && (mv->type == asd->type1 || mv->type == asd->type2)) {
        d = d * 3 / 2;
    }

    /* 属性克制。0x 直接归零，不走后面的截断。 */
    uint8_t eff = type_effectiveness(mv->type, def_s);
    if (eff == 0) {
        return 0;
    }
    d = d * eff / 10;

    if (d > 999) d = 999;
    if (d < 1) d = 1;
    return (uint16_t)d;
}

/* 旧接口：不带能力等级，供已有单测与 AI 估算使用。 */
uint16_t calc_damage(const Statsheet *atk_s, Condition atk_cond,
                     const Statsheet *def_s, Condition def_cond,
                     const MoveDef *mv, Weather wx, int16_t crit_percent)
{
    return calc_damage_ex(atk_s, atk_cond, NULL,
                          def_s, def_cond, NULL,
                          mv, wx, crit_percent, NULL);
}

/* ---------- 抽签源 ---------- */

/*
 * 伪随机抽签（xorshift32）。ESP32 有硬件 RNG，但它会阻塞且需要 seed，
 * 对单机对战这种低频场景不划算。
 *
 * 单测会提供同名强符号覆盖本实现，以便把伤害断言固定下来。
 */
#ifndef POKE_CALC_TEST_NO_RAND
static uint32_t s_rng = 0x2545F491u;

uint8_t calc_rand_85_100(void)
{
    s_rng ^= s_rng << 13;
    s_rng ^= s_rng >> 17;
    s_rng ^= s_rng << 5;
    return (uint8_t)(85 + (s_rng % 16));
}
#endif

/* ---------- 初始化 ---------- */

void statsheet_init(Statsheet *s, uint16_t species_id, uint8_t level,
                    uint8_t nature)
{
    s->species_id = species_id;
    s->level = level ? level : 50;
    s->nature = nature;
    s->ability = ABIL_NONE;
    s->item = 0;
    s->mega = 0;
    for (int i = 0; i < 6; i++) {
        s->ey.iv[i] = MAX_IV;
        s->ey.ev[i] = 0;
    }
}

void statsheet_set_mega(Statsheet *s, uint16_t mega_pid)
{
    s->species_id = mega_pid;
    s->mega = 1;
}

void statsheet_set_ev(Statsheet *s, uint8_t stat_idx, uint8_t value)
{
    /* 1-based，必须同时挡上下界（0 会写到 ev[-1] == iv[5]） */
    if (stat_idx < 1 || stat_idx > 6) {
        return;
    }
    /* value 是 uint8_t，MAX_EV 也是 255，所以这个 clamp 恒不触发。
     * 保留是为了接口语义完整（调用方传 uint16_t 时仍会被截断到 255）。 */
    s->ey.ev[stat_idx - 1] = value;
}

void statsheet_unmega(Statsheet *s)
{
    if (!s->mega) {
        return;
    }
    uint16_t base = mega_to_base(s->species_id);
    if (base) {
        s->species_id = base;
    }
    s->mega = 0;
}
