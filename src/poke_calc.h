/*
 * poke_calc.h - 能力值计算与伤害公式（第六代规则）
 *
 * 公式要点（Gen VI，X/Y ORAS）
 * ------------------------
 * 能力值
 *   HP  = ((Base_HP + IV/4) * 2) * Level / 100 + Level + 10 + EV/4
 *   其它 = ((Base + IV/4) * 2) * Level / 100 + 5 + EV/4
 *   之后乘性格修正（该项 +10% / -10%）
 *
 * 伤害（Gen VI 与 Gen III 有三处实质差异，务必注意）
 *   BaseDamage = floor(floor(floor(2*Level/5 + 2) * A * P / D) / 50) + 2
 *
 *   之后依次乘：
 *     天气       火/水 1.5 或 0.5（沙暴/冰雹**不影响伤害**，
 *               沙暴只把岩石系特防 *1.5，那部分在能力值层处理）
 *     暴击       1.5（Gen III 是 2.0）
 *     随机       85..100 / 100（Gen III 是 217..255 / 255）
 *     STAB       1.5
 *     属性克制   18x18 表（0 / 0.25 / 0.5 / 1 / 2 / 4）
 *     灼伤       物理招且攻击方灼伤 -> 0.5
 *     能力等级   攻/防各自的 -6..+6 修正
 *
 *   乘法顺序会影响整数截断结果，Gen VI 官方顺序为
 *     base -> 天气 -> 暴击 -> 随机 -> STAB -> 克制 -> 灼伤
 *
 * 所有中间量用 uint32_t；最终截断到 uint16_t。
 *
 * 本模块是纯计算，不持有任何战斗状态：能力等级、天气、异常状态
 * 都由调用方（poke_battle）传入。
 */

#ifndef POKE_CALC_H
#define POKE_CALC_H

#include <stdbool.h>
#include <stdint.h>

#include "poke_abilities.h"
#include "poke_items.h"
#include "poke_moves.h"
#include "poke_natures.h"
#include "poke_species.h"
#include "poke_types.h"

#define MAX_IV 31
#define MAX_EV 255
#define MAX_STAGE 6

/* 努力值与个体值的存储：6 项各 8 bit刚好 6 字节 */
typedef struct {
    uint8_t iv[6]; /* 1=hp 2=atk 3=def 4=spa 5=spd 6=spe */
    uint8_t ev[6];
} EffortYield;

typedef struct {
    uint16_t species_id; /* pokemon id */
    uint8_t level;       /* 1..100 */
    uint8_t nature;      /* 0..25，对应 kNatures */
    uint8_t ability;     /* AbilityKind */
    uint16_t item;       /* ItemDef.id，0=无道具 */
    uint8_t mega;        /* 0=普通 1=Mega */
    EffortYield ey;
} Statsheet;

/* 战斗中的临时状态 */
typedef enum {
    WX_NONE = 0,
    WX_RAIN,
    WX_SUN,
    WX_SAND,
    WX_HAIL,
} Weather;

typedef enum {
    CON_NONE = 0,
    CON_PARALYZE,
    CON_POISON,
    CON_BURN,
    CON_FREEZE,
    CON_SLEEP,
} Condition;

/* ---- 能力值 ---- */

uint16_t calc_hp(const Statsheet *s);
uint16_t calc_stat(const Statsheet *s, uint8_t stat_idx);

/* 带能力等级的能力值。stage 传 NULL 视为 0。
 * stat_idx: 2=atk 3=def 4=spa 5=spd 6=spe（HP 不吃等级） */
uint16_t calc_stat_staged(const Statsheet *s, uint8_t stat_idx, int8_t stage);

/* ---- 天气 / 条件 ---- */

uint16_t calc_hp_max(const Statsheet *s);

/* ---- 伤害 ---- */

/*
 * 单次伤害，不修改任何状态。
 *
 * atk_stage / def_stage：攻防双方的能力等级数组（索引 0..5 对应
 * hp/atk/def/spa/spd/spe，HP 项忽略）。传 NULL 视为全 0。
 * 暴击时忽略防守方的能力等级，忽略攻方灼伤。
 *
 * crit_percent 用千分比（1000 = 10%），传入 -1 表示强制暴击、
 * 0 表示不暴击；返回 1 表示实际暴击、0 表示未暴击（调用方
 * 需要知道是否暴击以决定提示文案）。
 *
 * CALC_NO_ROLL（-2）：不暴击且随机因子取 100（不抽签）。
 * AI 预估伤害必须用它——否则每次预估都被 85~100 随机因子污染，
 * 同一局面两次算出不同的数，决策不可重放，评估函数也不再
 * 满足 eval(0) == -eval(1) 的对称性。
 */
#define CALC_NO_ROLL ((int16_t)-2)
uint16_t calc_damage_ex(const Statsheet *atk_s, Condition atk_cond,
                        const int8_t *atk_stage,
                        const Statsheet *def_s, Condition def_cond,
                        const int8_t *def_stage,
                        const MoveDef *mv, Weather wx, int16_t crit_percent,
                        int *did_crit);

/* 旧接口：不带能力等级，兼容已有单测 */
uint16_t calc_damage(const Statsheet *atk_s, Condition atk_cond,
                     const Statsheet *def_s, Condition def_cond,
                     const MoveDef *mv, Weather wx, int16_t crit_percent);

/* 属性克制倍率，单位 10（10=1.0x, 20=2.0x, 5=0.5x, 0=0x） */
uint8_t type_effectiveness(uint8_t move_type, const Statsheet *def);

/* ---- 初始化辅助 ---- */

/* 抽签源：返回 85~100 的随机因子（Gen VI）。
 * 抽成独立函数是为了单测时可替换成固定 seed，
 * 让伤害断言可重放。实现见 poke_calc.c。*/
uint8_t calc_rand_85_100(void);

void statsheet_init(Statsheet *s, uint16_t species_id, uint8_t level,
                    uint8_t nature);

/* 按 Mega 形态重设species_id（换人不换等级性格努力值） */
void statsheet_set_mega(Statsheet *s, uint16_t mega_pid);

/* 设置努力值并返回实际生效值（三代有 10/25/50/100 上限，本项目不做上限） */
void statsheet_set_ev(Statsheet *s, uint8_t stat_idx, uint8_t value);

/* 把 species_id 从 Mega 形态还原成基础形态 */
void statsheet_unmega(Statsheet *s);

#endif /* POKE_CALC_H */