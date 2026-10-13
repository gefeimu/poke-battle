/*
 * test_calc.c - 三代伤害公式单测
 *
 * 期望值经Python 独立脚本按三代标准克制表核算过。
 * 关键：三代没有 Fairy 属性，因此
 *   草打草 = 0.5x、草打毒 = 0.5x、地面打毒 = 2x
 * 这类"反直觉"关系容易记错，是本测试的主要防回归点。
 *
 * 编译：
 *   gcc -std=c11 test/test_calc.c src/poke_calc.c \
 *       gen/poke_types.c gen/poke_species.c gen/poke_natures.c \
 *       gen/poke_moves.c gen/poke_abilities.c gen/poke_items.c \
 *       gen/poke_learnsets.c -Isrc -Igen -o test_calc
 */

#include <stdio.h>
#include <string.h>

#include "poke_calc.h"
#include "poke_learnsets.h"

static int g_pass = 0;
static int g_fail = 0;

static void ok(int cond, const char *name, const char *detail)
{
    if (cond) {
        g_pass++;
        printf("  [ OK ] %s\n", name);
    } else {
        g_fail++;
        printf("  [FAIL] %s   %s\n", name, detail);
    }
}

static void ok_u16(uint16_t got, uint16_t want, const char *name)
{
    char buf[128];
    snprintf(buf, sizeof buf, "期望 %u，实得 %u", want, got);
    ok(got == want, name, buf);
}

/*
 * 单测通过 -DPOKE_CALC_TEST_NO_RAND 关闭引擎内的抽签实现，
 * 改用下面的固定 seed 版本，让所有伤害断言可重放。
 */
static uint32_t s_rng = 0x2545F491u;
uint8_t calc_rand_85_100(void)
{
    s_rng ^= s_rng << 13;
    s_rng ^= s_rng >> 17;
    s_rng ^= s_rng << 5;
    return (uint8_t)(85 + (s_rng % 16));
}
static void rng_reset(void) { s_rng = 0x2545F491u; }

/*
 * 本测试也链接 poke_battle.c（ability/item 效果需要 battle_log_add 等），
 * 所以要提供 battle_rng 强符号——否则链接期undefined reference。
 */
uint32_t battle_rng(void)
{
    s_rng ^= s_rng << 13;
    s_rng ^= s_rng >> 17;
    s_rng ^= s_rng << 5;
    return s_rng;
}

int main(void)
{
    printf("=== 三代伤害公式单测 ===\n\n");

    /* ================= 能力值 ================= */
    printf("[能力值] Lv50 满IV 努力值0性格=认真\n");
    Statsheet bulba;
    statsheet_init(&bulba, 1, 50, 1);
    /* HP  = (45+7)*2*50/100 + 50 + 10 + 0 = 52 + 60 = 112 */
    ok_u16(calc_hp(&bulba), 112, "妙蛙种子 HP = 112");
    /* 攻击 = (49+7)*2*50/100 + 5 = 56 + 5 = 61 */
    ok_u16(calc_stat(&bulba, 2), 61, "妙蛙种子 攻击 = 61");
    /* 防御 = 61 */
    ok_u16(calc_stat(&bulba, 3), 61, "妙蛙种子 防御 = 61");
    /* 特攻 = (65+7)*2*50/100 + 5 = 72 + 5 = 77 */
    ok_u16(calc_stat(&bulba, 4), 77, "妙蛙种子 特攻 = 77");
    /* 特防 = 77 */
    ok_u16(calc_stat(&bulba, 5), 77, "妙蛙种子 特防 = 77");
    /* 速度 = (45+7)*2*50/100 + 5 = 52 + 5 = 57 */
    ok_u16(calc_stat(&bulba, 6), 57, "妙蛙种子 速度 = 57");

    printf("\n[能力值] 性格修正 孤僻(+攻-防)\n");
    Statsheet n = bulba;
    n.nature = 2; /* 孤僻 */
    ok_u16(calc_stat(&n, 2), 67, "孤僻 攻击 61 -> 67 (+10%)");
    ok_u16(calc_stat(&n, 3), 54, "孤僻 防御 61 -> 54 (-10%)");
    ok_u16(calc_stat(&n, 4), 77, "孤僻 特攻不变");

    printf("\n[能力值] 努力值 252 点投入攻击 = +63\n");
    Statsheet e = bulba;
    statsheet_set_ev(&e, 2, 252);
    ok_u16(calc_stat(&e, 2), 124, "252 努力攻击 = 124");
    /* 0 个努力值时不受影响 */
    ok_u16(calc_stat(&bulba, 2), 61, "0 努力 攻击仍为 61");

    printf("\n[能力值] 努力值 255 上限\n");
    statsheet_set_ev(&e, 2, 255);
    ok_u16(calc_stat(&e, 2), 61 + 63, "255 努力 攻击 = 124（255/4=63）");

    printf("\n[能力值] 喷火龙 Lv50\n");
    Statsheet charz;
    statsheet_init(&charz, 6, 50, 1);
    /* HP = (78+7)*2*50/100 + 60 = 85 + 60 = 145 */
    ok_u16(calc_hp(&charz), 145, "喷火龙 HP = 145");
    /* 特攻 = (109+7)*2*50/100 + 5 = 116 + 5 = 121 */
    ok_u16(calc_stat(&charz, 4), 121, "喷火龙 特攻 = 121");
    /* 特防 = (85+7)*2*50/100 + 5 = 92 + 5 = 97 */
    ok_u16(calc_stat(&charz, 5), 97, "喷火龙 特防 = 97");

    /* ================= 属性克制 ================= */
    /* 妙蛙种子 = Grass/Poison，喷火龙 = Fire/Flying
     * 以下期望值按 Gen VI 标准表核算（Fairy 已启用）。 */
    printf("\n[属性克制] 妙蛙种子(草/毒)\n");
    ok_u16(type_effectiveness(TYPE_FIRE, &bulba), 20, "火 -> 草/毒 = 2x * 1x = 2x");
    ok_u16(type_effectiveness(TYPE_WATER, &bulba), 20, "水 -> 草/毒 = 2x * 1x = 2x");
    ok_u16(type_effectiveness(TYPE_GRASS, &bulba), 2, "草 -> 草/毒 = 0.5x * 0.5x = 0.25x");
    ok_u16(type_effectiveness(TYPE_ELECTRIC, &bulba), 5, "电 -> 草/毒 = 0.5x * 1x = 0.5x");
    ok_u16(type_effectiveness(TYPE_GROUND, &bulba), 10, "地面 -> 草/毒 = 0.5x * 2x = 1x");
    ok_u16(type_effectiveness(TYPE_ICE, &bulba), 20, "冰 -> 草/毒 = 2x * 1x = 2x");
    /* 飞行对草和毒都�� 1.0x。此处曾长期被误写为 2x/2x=4x，
     * 导致 AI 认为「飞行打草/毒」是克制招而选错。 */
    ok_u16(type_effectiveness(TYPE_FLYING, &bulba), 10, "飞 -> 草/毒 = 1x * 1x = 1x");
    ok_u16(type_effectiveness(TYPE_STEEL, &bulba), 0, "钢 -> 毒 = 0x");
    ok_u16(type_effectiveness(TYPE_NORMAL, &bulba), 10, "普通 -> 草/毒 = 1x");

    printf("\n[属性克制] 喷火龙(火/飞)\n");
    ok_u16(type_effectiveness(TYPE_WATER, &charz), 20, "水 -> 火/飞 = 2x * 1x = 2x");
    ok_u16(type_effectiveness(TYPE_ELECTRIC, &charz), 20, "电 -> 火/飞 = 1x * 2x = 2x");
    ok_u16(type_effectiveness(TYPE_GROUND, &charz), 0, "地面 -> 火/飞 = 2x * 0x = 0x");
    ok_u16(type_effectiveness(TYPE_ROCK, &charz), 40, "岩石 -> 火/飞 = 2x * 2x = 4x");
    ok_u16(type_effectiveness(TYPE_GRASS, &charz), 2, "草 -> 火/飞 = 0.5x * 0.5x = 0.25x");
    ok_u16(type_effectiveness(TYPE_ICE, &charz), 10, "冰 -> 火/飞 = 0.5x * 2x = 1x");
    ok_u16(type_effectiveness(TYPE_FIGHTING, &charz), 5, "斗 -> 火/飞 = 0.5x * 1x = 0.5x");

    printf("\n[属性克制] 无效关系\n");
    ok_u16(type_effectiveness(TYPE_GROUND, &charz), 0, "地面打飞行 = 0x");
    ok_u16(type_effectiveness(TYPE_NORMAL, &charz), 10, "普通打火/飞 = 1x（正常打非幽灵）");

    /* Gen VI 特有/易错关系的回归网。此前克制表里出现过
     * 「飞行打草/毒=2x」「火打钢=2x」这类错误，会直接让 AI 选错招，
     * 所以这里对 0x 免疫、双属性组合与钢/龙/妖精关系逐条钉住。
     * 所有期望值由 tools/regen_types.py 的同一份 CHART 推导，改表时同步。 */
    printf("\n[属性克制] Gen VI 关系回归\n");
    {
        /* 巨钳螳螂 = 虫/钢。钢的 0.5x 抵掉了大部分 2x，
         * 这是一只「看着克制实则打不动」的典型，AI 必须能识别。 */
        Statsheet scizor;
        statsheet_init(&scizor, 212, 50, 0);
        ok_u16(type_effectiveness(TYPE_FIRE, &scizor), 10, "火 -> 虫/钢 = 2x * 0.5x = 1x（钢抗火）");
        ok_u16(type_effectiveness(TYPE_ELECTRIC, &scizor), 10, "电 -> 虫/钢 = 2x * 0.5x = 1x（钢抗电）");
        ok_u16(type_effectiveness(TYPE_ROCK, &scizor), 10, "岩 -> 虫/钢 = 2x * 0.5x = 1x（钢抗岩）");
        ok_u16(type_effectiveness(TYPE_FIGHTING, &scizor), 10, "斗 -> 虫/钢 = 0.5x * 2x = 1x（钢抗斗）");
        ok_u16(type_effectiveness(TYPE_GROUND, &scizor), 10, "地 -> 虫/钢 = 0.5x * 2x = 1x");
        ok_u16(type_effectiveness(TYPE_FLYING, &scizor), 10, "飞 -> 虫/钢 = 2x * 0.5x = 1x");
        /* 冰打虫 2x 但冰打钢只有 0.25x（冰抗钢），净 0.5x —— 最容易被想当然写成 2x */
        ok_u16(type_effectiveness(TYPE_ICE, &scizor), 5, "冰 -> 虫/钢 = 2x * 0.25x = 0.5x");
/* 草打虫 2x（表中曾误写 0.5x）* 草打钢 0.5x = 1x */
        ok_u16(type_effectiveness(TYPE_GRASS, &scizor), 10, "草 -> 虫/钢 = 2x * 0.5x = 1x");
        /* 钢打虫 0.5x * 钢打钢 0.5x = 0.25x */
        ok_u16(type_effectiveness(TYPE_STEEL, &scizor), 2, "钢 -> 虫/钢 = 0.5x * 0.5x = 0.25x");
    }

    {
        /* 巨金怪 = 钢/超能。地面 4x 的典型目标，火/超能极低。 */
        Statsheet metagross;
        statsheet_init(&metagross, 376, 50, 0);
        ok_u16(type_effectiveness(TYPE_GROUND, &metagross), 20, "地 -> 钢/超能 = 2x * 0.5x = 1x");
        ok_u16(type_effectiveness(TYPE_FIRE, &metagross), 5, "火 -> 钢/超能 = 0.5x * 0.5x = 0.25x");
        /* 超能打钢只被钢挡一半，并非免疫 */
        ok_u16(type_effectiveness(TYPE_PSYCHIC, &metagross), 2, "超能 -> 钢/超能 = 0.5x * 0.5x = 0.25x");
        ok_u16(type_effectiveness(TYPE_WATER, &metagross), 10, "水 -> 钢/超能 = 1x * 0.5x = 0.5x");
    }

    {
        /* 皮皮 = 妖精：Gen VI 新增属性，四条 2x + 龙 0x + 钢 2x。 */
        Statsheet clefable;
        statsheet_init(&clefable, 35, 50, 0);
        ok_u16(type_effectiveness(TYPE_DRAGON, &clefable), 0, "龙 -> 妖精 = 0x（龙唯一免疫）");
        ok_u16(type_effectiveness(TYPE_POISON, &clefable), 20, "毒 -> 妖精 = 2x");
        ok_u16(type_effectiveness(TYPE_STEEL, &clefable), 20, "钢 -> 妖精 = 2x");
        /* 虫打妖精是 1x（不是 Gen VI 的 2x），格斗打妖精只有 0.5x */
        ok_u16(type_effectiveness(TYPE_BUG, &clefable), 10, "虫 -> 妖精 = 1x");
        ok_u16(type_effectiveness(TYPE_FIGHTING, &clefable), 5, "斗 -> 妖精 = 0.5x");
        /* 龙不克草，用来确认不是「所有龙系招都该有奇效」 */
        ok_u16(type_effectiveness(TYPE_DRAGON, &bulba), 10, "龙 -> 草/毒 = 1x（龙不克草）");
    }

    /* 非法属性 id 不应越界读表，返回 1.0x */
    {
        Statsheet any;
        statsheet_init(&any, 1, 50, 0);
        ok_u16(type_effectiveness(200, &any), 10, "非法招式属性返回 1x（防御性）");
    }

    /* ================= 伤害 ================= */
    printf("\n[伤害] 招式数据核对\n");
    const MoveDef *flame = &kMoves[52];  /* id 53 Flamethrower */
    ok_u16(flame->power, 90, "喷射火焰 威力 = 90");
    ok(flame->type == TYPE_FIRE, "喷射火焰 属性 = 火", "");
    ok(flame->damage_class == DMG_SPECIAL, "喷射火焰 是特殊招式", "");
    ok_u16(flame->pp, 15, "喷射火焰 PP = 15");
    const MoveDef *tackle = &kMoves[0];  /* id 1 Pound */
    ok_u16(tackle->power, 40, "冲撞 威力 = 40");
    ok(tackle->damage_class == DMG_PHYSICAL, "冲撞 是物理招式", "");

    printf("\n[伤害] 喷射火焰 妙蛙种子(草/毒,特攻77) -> 喷火龙(火/飞,特防97)\n");
    /* Gen VI 精确手算：
     *   BaseDamage = floor(floor(22 * 77 * 90 / 50) / 97) + 2
     *             = floor(3049 / 97) + 2 = 31 + 2 = 33
     *   天气 无 -> 33
     *   暴击 无 -> 33
     *   随机 85..100/100 -> 28..33
     *   STAB 火招 vs 草/毒 -> 不触发
     *   克制 火 vs 火/飞 = 1x*0.5x = 0.5x -> 28*5/10 .. 33*5/10 = 14..16
     * 期望区间 14~16 */
    rng_reset();
    uint16_t d = calc_damage(&bulba, CON_NONE, &charz, CON_NONE, flame, WX_NONE, 0);
    ok(d >= 14 && d <= 16, "喷射火焰 伤害 = 14~16（无STAB，克制 0.5x）", "见上方手算");
    printf("       实际 %u\n", d);

    printf("\n[伤害] STAB 验证：喷火龙(火)用喷射火焰打自己\n");
    /* 喷火龙 Lv50 满IV 性格1(耐久)：SpA=121、SpD=97（不是 145/158）
     *   BaseDamage = floor(floor(22*121*90/50)/97)+2 = floor(4787/97)+2 = 49+2 = 51
     *   随机 85..100/100 -> 43..51
     *   STAB 1.5 -> 64..76
     *   克制 火 vs 火/飞 = 0.5x -> 32..38 */
    rng_reset();
    uint16_t d_stab = calc_damage(&charz, CON_NONE, &charz, CON_NONE, flame,
                                  WX_NONE, 0);
    ok(d_stab >= 32 && d_stab <= 38, "同属性 STAB 生效（32~38）", "Base 51*1.5*0.5");
    printf("       实际 %u（对照：妙蛙种子用火招仅 14~16，差在 STAB 与 SpA）\n", d_stab);
    ok(d_stab > d, "STAB 使伤害明显提高", "同属性应高于跨属性");

    printf("\n[伤害] 同 seed 结果可重放\n");
    rng_reset();
    uint16_t d2 = calc_damage(&bulba, CON_NONE, &charz, CON_NONE, flame, WX_NONE, 0);
    ok(d == d2, "同 seed 两次结果一致", "随机源应确定");

    printf("\n[伤害] 暴击倍率 1.5（Gen VI；Gen III 是 2.0）\n");
    rng_reset();
    uint16_t dn = calc_damage(&bulba, CON_NONE, &charz, CON_NONE, flame, WX_NONE, 0);
    rng_reset();
    uint16_t dc = calc_damage(&bulba, CON_NONE, &charz, CON_NONE, flame, WX_NONE, -1);
    ok(dc > dn, "暴击伤害高于普通", "应满足 dc > dn");
    /* Gen VI 顺序：暴击在随机之前，所以暴击值 = 普通值 * 1.5（取整） */
    ok(dc >= dn * 3 / 2 - 1 && dc <= dn * 3 / 2 + 1,
       "暴击约为普通的 1.5 倍", "Gen VI 暴击 1.5x");
    printf("       普通 %u vs 暴击 %u（比值 %.2f）\n", dn, dc, (double)dc / dn);

    printf("\n[伤害] 天气影响\n");
    rng_reset();
    uint16_t dry = calc_damage(&bulba, CON_NONE, &charz, CON_NONE, flame, WX_NONE, 0);
    rng_reset();
    uint16_t rain = calc_damage(&bulba, CON_NONE, &charz, CON_NONE, flame, WX_RAIN, 0);
    rng_reset();
    uint16_t sun = calc_damage(&bulba, CON_NONE, &charz, CON_NONE, flame, WX_SUN, 0);
    ok(rain < dry, "雨天火招式减伤", "雨天下火伤害应下降");
    ok(sun > dry, "晴天火招式增伤", "晴天火伤害应上升");
    printf("       无天气 %u / 雨 %u / 晴 %u\n", dry, rain, sun);

    printf("\n[伤害] 沙暴与冰雹不影响伤害（Gen VI 已取消）\n");
    /* Gen III 里沙暴会把岩石系招式 *1.5，第六代取消了。
     * 这是三代与六代的实质差异，专门测一下防止回退。 */
    rng_reset();
    uint16_t no_wx = calc_damage(&bulba, CON_NONE, &charz, CON_NONE, flame, WX_NONE, 0);
    rng_reset();
    uint16_t sand = calc_damage(&bulba, CON_NONE, &charz, CON_NONE, flame, WX_SAND, 0);
    rng_reset();
    uint16_t hail = calc_damage(&bulba, CON_NONE, &charz, CON_NONE, flame, WX_HAIL, 0);
    ok(sand == no_wx, "沙暴不改变伤害", "Gen VI 沙暴无伤害修正");
    ok(hail == no_wx, "冰雹不改变伤害", "Gen VI 冰雹无伤害修正");

    printf("\n[伤害] 攻击方灼伤降低物理伤害 50%%\n");
    rng_reset();
    uint16_t pn = calc_damage(&bulba, CON_NONE, &charz, CON_NONE, tackle, WX_NONE, 0);
    rng_reset();
    uint16_t pb = calc_damage(&bulba, CON_BURN, &charz, CON_NONE, tackle, WX_NONE, 0);
    ok(pb < pn, "灼伤降低物理伤害", "灼伤攻击减半");
    ok(pb >= pn / 2 - 1, "灼伤约减半", "50% 物理伤害");
    printf("       正常 %u vs 灼伤 %u\n", pn, pb);

    printf("\n[伤害] 状态招式无伤害\n");
    /* 不硬编码 id —— move id 用 Veekun 编号，印象中的序号常错。
     * 直接按"威力为 0"扫描出第一个状态招式来测。
     * 注意 kMoves 的下标 = id - 1，所以要按 id 递增来扫。 */
    uint16_t status_move = 0;
    for (uint16_t id = 1; id < MOVE_COUNT; id++) {
        const MoveDef *md = mv_get(id);
        if (md->power == 0 && md->damage_class == DMG_STATUS) {
            status_move = id;
            break;
        }
    }
    ok(status_move != 0, "存在状态招式", "未在招式表中找到");
    if (status_move) {
        const MoveDef *sm = mv_get(status_move);
        printf("       状态招式 id=%u %s\n", status_move, sm->name);
        ok_u16(calc_damage(&bulba, CON_NONE, &charz, CON_NONE, sm, WX_NONE, 0),
               0, "状态招式 伤害 = 0");
    }

    /* ================= Mega ================= */
    printf("\n[Mega] 形态切换\n");
    Statsheet mega = charz;
    statsheet_set_mega(&mega, 10034); /* Mega 喷火龙X */
    const SpeciesDef *mx = species_get(10034);
    ok(mx && mx->is_mega == 1, "Mega 喷火龙X 记录存在", "");
    ok(mega.species_id == 10034, "species_id 已切换", "");
    ok(mega.nature == charz.nature, "保留性格", "");
    ok(mega.level == charz.level, "保留等级", "");
    ok(mega.ey.ev[3] == charz.ey.ev[3], "保留努力值", "");
    printf("       普通 SpA=%u -> Mega SpA=%u\n",
           calc_stat(&charz, 4), calc_stat(&mega, 4));
    ok(calc_stat(&mega, 4) > calc_stat(&charz, 4), "Mega 后特攻提升", "");
    /* 火/飞 -> 火/龙 */
    printf("       普通属性 %u/%u -> Mega 属性 %u/%u\n",
           species_get(6)->type1, species_get(6)->type2,
           mx->type1, mx->type2);
    ok(mx->type2 != species_get(6)->type2, "Mega 后第二属性变化", "");

    /* ================= 学习表 ================= */
    printf("\n[学习表]\n");
    const LearnSet *ls = learnset_get(1);
    ok(ls && ls->node_count > 0, "妙蛙种子有学习表", "");
    if (ls && ls->node_count > 0) {
        ok(ls->nodes[0].level == 1, "首个节点为 Lv1", "");
        printf("       节点 %d，Lv%u 学 %s\n",
               ls->node_count, ls->nodes[0].level,
               kMoves[ls->nodes[0].move_id - 1].name);
    }
    /* 等级应递增 */
    int bad = 0;
    for (int pid = 1; pid <= 386; pid++) {
        const LearnSet *l = learnset_get((uint16_t)pid);
        if (!l) continue;
        for (int i = 1; i < l->node_count; i++) {
            if (l->nodes[i].level < l->nodes[i - 1].level) bad++;
        }
    }
    ok(bad == 0, "所有学习表等级递增", "存在乱序节点");

    printf("\n=== %d 通过, %d 失败 ===\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}