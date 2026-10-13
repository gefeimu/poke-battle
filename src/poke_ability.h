/*
 * poke_ability.h - 特性效果接口
 *
 * Veekun 没有机读的特性效果数据（ability_effect_text 表 404，
 * ability_changelog 只有 id 对照），所以效果全部手写。
 * 只实现对对战有实质影响的特性，其余返回"无效果"。
 */

#ifndef POKE_ABILITY_H
#define POKE_ABILITY_H

#include <stdbool.h>
#include <stdint.h>

#include "poke_battle.h"
#include "poke_calc.h"

/* ---- 特性分类查询 ---- */

/* 是否为"出场时改变天气"类：降雨/干旱/扬沙/降雪 */
bool ability_is_weather_setter(uint8_t kind);

/* 降雨特性返回 WX_RAIN，以此类推；非天气特性返回 WX_NONE */
Weather ability_weather(uint8_t kind);

/* ---- 战斗钩子 ---- */

/* 出场时触发（威吓降攻、天气特性设天气）。
 * 在 battle_start 与每次换人后调用。 */
void ability_on_switch_in(Battle *b, int side_is_foe);

/* 回合末触发（加速加速、自然回复回血）。在 end_of_turn 内调用。 */
void ability_on_end_turn(Battle *b, int side_is_foe);

/* ---- 数值修正 ---- */

/* 攻击/特攻倍率修正（大力士/纯粹力量翻倍）。返回 100 = 不变 */
uint16_t ability_atk_mult(uint8_t kind);
uint16_t ability_spa_mult(uint8_t kind);

/* 防御类修正（神奇鳞片：异常时防御 1.5x；厚脂肪：火/冰招减半） */
uint16_t ability_def_mult(const BattleMon *m, uint8_t kind);
uint16_t ability_spd_mult(const BattleMon *m, uint8_t kind);

/* 速度修正（叶绿素晴天翻倍、悠游自如雨天翻倍） */
uint16_t ability_speed_mult(const BattleMon *m, uint8_t kind, Weather wx);

/* 招式威力修正（破格/适应力/技师/猛火/激流/茂盛/虫之预感） */
uint16_t ability_power_mult(const BattleMon *atk, uint8_t kind,
                            const MoveDef *mv);

/* ---- 免疫与交互 ---- */

/* 是否免疫某异常（根性/ Own Tempo 免疫混乱等） */
bool ability_immune_ailment(const BattleMon *m, uint8_t kind, uint8_t ailment);

/* 是否免疫某属性招式（飘浮免疫地面、储水免疫水、蓄电免疫电） */
bool ability_immune_type(const BattleMon *m, uint8_t kind, uint8_t move_type);

/* 是否免疫能力下降（白色烟雾/恒净之躯/胸甲） */
bool ability_immune_stat_drop(uint8_t kind);

/* 是否免疫一击必杀（结实） */
bool ability_immune_ohko(uint8_t kind);

/* ---- 特性名 ---- */

/*
 * 特性名，**中文**。这个字符串会原样进战斗日志、再显示在对话框里，
 * 所以不能用 kAbilities[].name（那是英文标识符，写进中文界面很奇怪）。
 */
const char *ability_name(uint8_t kind);

#endif /* POKE_ABILITY_H */
