/*
 * poke_item.h - 道具效果接口
 *
 * Veekun 无机读的道具效果数据（item_effects.csv 404、
 * item_flavor_text 只覆盖球类），所以效果数值在
 * tools/gen_gen3_data.py 的 ITEM_EFFECTS 表里手写。
 * 本模块负责把那些数值接进战斗流程。
 */

#ifndef POKE_ITEM_H
#define POKE_ITEM_H

#include <stdbool.h>
#include <stdint.h>

#include "poke_battle.h"
#include "poke_calc.h"
#include "poke_items.h"

/* ---- 数值查询 ---- */

/* 携带道具对某属性招式的威力加成百分比（20 = +20%） */
uint16_t item_type_boost(uint16_t item_id, uint8_t move_type);

/* 携带道具的额外暴击等级（焦点镜片 = 1） */
int item_crit_stage(uint16_t item_id);

/* ---- 战斗钩子 ---- */

/* 回合末：吃剩的东西回血 */
void item_on_end_turn(Battle *b, int side_is_foe);

/* 造成伤害后：贝壳之铃回血、反伤类道具 */
void item_on_deal_damage(Battle *b, int attacker_is_foe, uint16_t dmg);

/* ---- 概率与免疫 ---- */

/* 快捷腰带：本回合是否获得先手 */
bool item_quick_claw_proc(Battle *b, int side_is_foe);

/* 是否免疫能力下降 */
bool item_immune_stat_drop(uint16_t item_id);

/* 是否免疫畏缩 */
bool item_immune_flinch(uint16_t item_id);

/* 是否免疫会心一击 */
bool item_immune_crit(uint16_t item_id);

/* 攻击时使对手畏缩的概率（王者信物/尖钻影响） */
uint8_t item_flinch_pct(uint16_t item_id);

/* ---- 道具名 ---- */

const char *item_name(uint16_t item_id);

/* ---- Mega 石 ---- */

/* 携带的道具是否是"属于这只宝可梦"的 Mega 石（用于 battle_mega 校验） */
bool mon_has_mega_stone(const BattleMon *m);

#endif /* POKE_ITEM_H */
