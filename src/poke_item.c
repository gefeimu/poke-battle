/*
 * poke_item.c - 道具效果实现
 *
 * 效果数值来自 tools/gen_gen3_data.py 的 ITEM_EFFECTS 手写表
 * （Veekun 无机读数据）。本模块只负责把数值接进战斗流程。
 */

#include "poke_item.h"

#include <ctype.h>
#include <string.h>

/* ---------- 名字 ---------- */

const char *item_name(uint16_t item_id)
{
    const ItemDef *it = item_get(item_id);
    return it && it->name ? it->name : "None";
}

/* ---------- Mega 石归属校验 ---------- */

/*
 * 该宝可梦携带的道具是否是"属于它自己"的 Mega 石。
 *
 * 判定规则：道具分类为 ITEMKIND_MEGA_STONE，且石头名以
 * 基础形态的标识符开头（都是小写：charizardite x vs charizard）。
 * 官方命名只有一个不规则：赫拉克罗斯的石头叫 Heracronite
 * （不是 Heracrossite），单独列例外。
 *
 * 为什么需要它：battle_mega 只校验 mega_pid 与出战形态匹配，
 * 不校验道具。没有这道校验时，任何携带任何 Mega 石的宝可梦
 * 都能超级进化，AI 也会把"带错石头"当作可 Mega。
 */
bool mon_has_mega_stone(const BattleMon *m)
{
    const ItemDef *it = item_get(m->base.item);
    if (!it || it->kind != ITEMKIND_MEGA_STONE || !it->name) {
        return false;
    }
    const SpeciesDef *sd = species_get(m->base.species_id);
    if (!sd || !sd->name) {
        return false;
    }
    /* 石头名转小写后与物种标识符做前缀比较 */
    char stone[32];
    size_t n = strlen(it->name);
    if (n >= sizeof(stone)) {
        n = sizeof(stone) - 1;
    }
    for (size_t i = 0; i < n; i++) {
        stone[i] = (char)tolower((unsigned char)it->name[i]);
    }
    stone[n] = '\0';
    size_t sn = strlen(sd->name);
    if (strncmp(stone, sd->name, sn) == 0) {
        return true;
    }
    /* 官方命名例外 */
    if (strcmp(it->name, "Heracronite") == 0 && strcmp(sd->name, "heracross") == 0) {
        return true;
    }
    return false;
}

/* ---------- 数值查询 ---------- */

uint16_t item_type_boost(uint16_t item_id, uint8_t move_type)
{
    const ItemDef *it = item_get(item_id);
    if (!it || it->boost_type == 0) {
        return 100;
    }
    if (it->boost_type == move_type) {
        return (uint16_t)(100 + it->boost_amount);
    }
    return 100;
}

int item_crit_stage(uint16_t item_id)
{
    const ItemDef *it = item_get(item_id);
    return it ? it->crit_stage : 0;
}

/* ---------- 战斗钩子 ---------- */

void item_on_end_turn(Battle *b, int side_is_foe)
{
    BattleMon *m = battle_active(b, side_is_foe);
    if (!m || !m->alive || m->cur_hp >= m->hp_max) {
        return;
    }
    const ItemDef *it = item_get(m->base.item);
    if (!it || it->leftovers_pct == 0) {
        return;
    }
    uint16_t amount = m->hp_max * it->leftovers_pct / 100;
    if (amount < 1) {
        amount = 1;
    }
    m->cur_hp = (uint16_t)(m->cur_hp + amount);
    if (m->cur_hp > m->hp_max) {
        m->cur_hp = m->hp_max;
    }
    log_add(&b->log, "%s 靠 %s 回复了 %u 点 HP！",
            side_is_foe ? b->foe.name : b->me.name, item_name(m->base.item),
            amount);
}

void item_on_deal_damage(Battle *b, int attacker_is_foe, uint16_t dmg)
{
    if (dmg == 0) {
        return;
    }
    BattleMon *atk = battle_active(b, attacker_is_foe);
    if (!atk || !atk->alive) {
        return;
    }
    const ItemDef *it = item_get(atk->base.item);
    if (!it) {
        return;
    }
    /* 贝壳之铃：按造成伤害回血 */
    if (it->shell_bell_pct > 0 && atk->cur_hp < atk->hp_max) {
        uint16_t amount = dmg * it->shell_bell_pct / 100;
        if (amount < 1) {
            amount = 1;
        }
        atk->cur_hp = (uint16_t)(atk->cur_hp + amount);
        if (atk->cur_hp > atk->hp_max) {
            atk->cur_hp = atk->hp_max;
        }
        log_add(&b->log, "%s 靠 %s 回复了 %u 点 HP！",
                attacker_is_foe ? b->foe.name : b->me.name,
                item_name(atk->base.item), amount);
    }
    /* 反伤类道具（粘性钩等）：携带者造成伤害时按比例反噬自身 */
    if (it->chip_pct > 0) {
        uint16_t recoil = (uint16_t)((uint32_t)dmg * it->chip_pct / 100);
        if (recoil < 1) {
            recoil = 1;
        }
        if (recoil >= atk->cur_hp) {
            atk->cur_hp = 0;
            atk->alive = false;
            log_add(&b->log, "%s 被 %s 反噬，倒下了！",
                    attacker_is_foe ? b->foe.name : b->me.name,
                    item_name(atk->base.item));
        } else {
            atk->cur_hp = (uint16_t)(atk->cur_hp - recoil);
            log_add(&b->log, "%s 被 %s 反噬，受到 %u 点伤害！",
                    attacker_is_foe ? b->foe.name : b->me.name,
                    item_name(atk->base.item), recoil);
        }
    }
}

/* ---------- 概率与免疫 ---------- */

bool item_quick_claw_proc(Battle *b, int side_is_foe)
{
    BattleMon *m = battle_active(b, side_is_foe);
    if (!m) {
        return false;
    }
    const ItemDef *it = item_get(m->base.item);
    if (!it || it->quick_pct == 0) {
        return false;
    }
    return (battle_rng() % 100) < it->quick_pct;
}

bool item_immune_stat_drop(uint16_t item_id)
{
    const ItemDef *it = item_get(item_id);
    return it && it->immune_stat_drop;
}

bool item_immune_flinch(uint16_t item_id)
{
    const ItemDef *it = item_get(item_id);
    return it && it->immune_flinch;
}

bool item_immune_crit(uint16_t item_id)
{
    const ItemDef *it = item_get(item_id);
    return it && it->immune_crit;
}

uint8_t item_flinch_pct(uint16_t item_id)
{
    const ItemDef *it = item_get(item_id);
    return it ? it->flinch_pct : 0;
}
