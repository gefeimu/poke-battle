/* 由 tools/gen_gen3_data.py 自动生成 */
#ifndef POKE_ITEMS_H
#define POKE_ITEMS_H

#include <stdint.h>
#include <stddef.h>

typedef enum {
    ITEMKIND_NONE = 0,
    ITEMKIND_BERRY,
    ITEMKIND_HOLD,
    ITEMKIND_MEGA_STONE,
    ITEMKIND_TYPE_SPECIFIC,
    ITEMKIND_RESTORE,
} ItemKind;

typedef struct {
    uint16_t id;
    const char *name;
    const char *name_cn;   /* 简体中文名，UI 显示用 */
    uint8_t  kind;
    /* ---- 携带时的常驻效果 ---- */
    uint16_t heal;        /* 树果类：回复量 HP */
    uint8_t  boost_type; /* 增强某属性招式威力，0 = 无 */
    uint8_t  boost_amount;/* 增强百分比，20 = +20% */
    uint8_t  boost_stat; /* 增强某能力值，0=无 2=atk 3=def 4=spa 5=spd 6=spe */
    int8_t   boost_turns;
    uint8_t  stat_amount;/* 能力增强百分比 */
    /* ---- 概率与特殊效果 ---- */
    uint8_t  flinch_pct;  /* 携带者攻击时使对手畏缩的概率 */
    uint8_t  crit_stage;  /* 额外暴击等级 */
    uint8_t  focus_pct;   /* 焦点镜片类：会心率提升 */
    uint8_t  quick_pct;   /* 快捷腰带给先手概率 */
    uint8_t  leftovers_pct; /* 吃剩的东西：回合末回血百分比 */
    uint8_t  shell_bell_pct; /* 贝壳之铃：造成伤害回血百分比 */
    uint8_t  chip_pct;    /* 钢妖反伤（锐利之牙/铁刺）百分比 */
    /* ---- 状态免疫 ---- */
    uint8_t  immune_stat_drop; /* 1 = 防能力下降 */
    uint8_t  immune_crit;      /* 1 = 防会心一击 */
    uint8_t  immune_flinch;    /* 1 = 防畏缩 */
} ItemDef;

#define ITEM_COUNT 383
extern const ItemDef kItems[ITEM_COUNT];

/* 按道具 id 查定义，找不到返回 NULL */
const ItemDef *item_get(uint16_t item_id);

#endif