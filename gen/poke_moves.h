/* 由 tools/gen_gen3_data.py 自动生成 */
#ifndef POKE_MOVES_H
#define POKE_MOVES_H

#include <stdint.h>
#include "poke_types.h"

/* ---------- 招式大类（Veekun move_meta.meta_category_id） ---------- */
typedef enum {
    MCAT_DAMAGE,  /* 0 */
    MCAT_AILMENT,  /* 1 */
    MCAT_NET_GOOD_STATS,  /* 2 */
    MCAT_HEAL,  /* 3 */
    MCAT_DAMAGE_AILMENT,  /* 4 */
    MCAT_SWAGGER,  /* 5 */
    MCAT_DAMAGE_LOWER,  /* 6 */
    MCAT_DAMAGE_RAISE,  /* 7 */
    MCAT_DAMAGE_HEAL,  /* 8 */
    MCAT_OHKO,  /* 9 */
    MCAT_WHOLE_FIELD,  /* 10 */
    MCAT_FIELD,  /* 11 */
    MCAT_FORCE_SWITCH,  /* 12 */
    MCAT_UNIQUE,  /* 13 */
    MCAT_COUNT
} MoveCategory;

/* ---------- 异常状态（Veekun move_meta.meta_ailment_id） ---------- */
typedef enum {
    MAIL_UNKNOWN,  /* -1 */
    MAIL_NONE,  /* 0 */
    MAIL_PARALYSIS,  /* 1 */
    MAIL_SLEEP,  /* 2 */
    MAIL_FREEZE,  /* 3 */
    MAIL_BURN,  /* 4 */
    MAIL_POISON,  /* 5 */
    MAIL_CONFUSION,  /* 6 */
    MAIL_INFATUATION,  /* 7 */
    MAIL_TRAP,  /* 8 */
    MAIL_NIGHTMARE,  /* 9 */
    MAIL_TORMENT,  /* 12 */
    MAIL_DISABLE,  /* 13 */
    MAIL_YAWN,  /* 14 */
    MAIL_NO_TYPE_IMMUNITY,  /* 17 */
    MAIL_LEECH_SEED,  /* 18 */
    MAIL_PERISH_SONG,  /* 20 */
    MAIL_INGRAIN,  /* 21 */
    MAIL_PROTECT,  /* 43 */
    MAIL_COUNT
} MoveAilment;

typedef enum { DMG_STATUS = 0, DMG_PHYSICAL = 1, DMG_SPECIAL = 2 } DamageClass;

/* 能力变化：哪一项、变几级 */
typedef struct { uint8_t stat; int8_t change; } StatChange;

#define MOVE_MAX_STAT_CHANGES 5

typedef struct {
    uint16_t id;
    const char *name;
    const char *name_cn;   /* 简体中文名，UI 显示用 */
    uint8_t type;
    uint8_t power;
    uint8_t pp;
    uint8_t accuracy;      /* 0=必中 */
    int8_t  priority;       /* -6..+5 */
    uint8_t damage_class;
    uint8_t category;      /* MoveCategory */
    uint8_t ailment;       /* MoveAilment */
    uint8_t ailment_chance;/* 异常触发概率 %，0 = 不触发 */
    uint8_t stat_chance;   /* 能力变化概率 %，0 = 必定触发 */
    uint8_t n_stat_changes;
    StatChange stat_changes[MOVE_MAX_STAT_CHANGES];
    uint8_t min_hits;
    uint8_t max_hits;
    uint8_t heal;           /* 治疗/吸取，HP * heal / 100 */
    uint8_t crit_rate;      /* 0=普通 1=高 */
    uint8_t flinch_chance;
    uint8_t drain;          /* 反伤比例 % */
    uint8_t min_turns;      /* 蓄力/持续下限 */
    uint8_t max_turns;      /* 持续上限 */
    uint16_t effect_id;     /* Veekun 原始 effect_id，仅调试参考，勿用于逻辑 */
} MoveDef;

#define MOVE_COUNT 354

/*
 * 索引约定：kMoves[i].id == i + 1，即**下标 = move_id - 1**。
 * kMoves[0] 不是占位，而是 id 1 (Pound)。
 *
 * 因此绝不要写 kMoves[move_id] —— 那是下一个招式（move_id=MOVE_COUNT
 * 时直接越界读）。一律用下面的 mv_get()。
 */
extern const MoveDef kMoves[MOVE_COUNT];

/*
 * 按 move_id 取招式。id 非法（0 或 > MOVE_COUNT）返回 NULL。
 * 校验逻辑收敛在这里，避免每个调用点各自记错偏移。
 */
static inline const MoveDef *mv_get(uint16_t move_id)
{
    return (move_id >= 1 && move_id <= MOVE_COUNT) ? &kMoves[move_id - 1]
                                                   : NULL;
}

#endif