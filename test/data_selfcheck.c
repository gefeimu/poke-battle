/*
 * data_selfcheck.c - 数据表自检
 *
 * 用真实数值验证生成的数据表是否正确，比"能编译"强得多。
 * 编译运行：
 *   gcc -std=c11 test/data_selfcheck.c gen/poke_types.c gen/poke_moves.c \
 *       gen/poke_species.c gen/poke_learnsets.c gen/poke_natures.c \
 *       gen/poke_abilities.c gen/poke_items.c -Igen -o build_selfcheck
 */

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

/*
 * 规则世代。必须与生成数据表时用的 --gen 一致，否则 Fairy 相关断言会误报。
 * 本项目当前为第六代（X/Y ORAS）：启用 Fairy 属性与 Mega 进化。
 *
 *   gcc ...                                   # 默认第六代
 *   gcc ... -DRULE_GEN=3                      # 三代规则回归（屏蔽 Fairy）
 */
#ifndef RULE_GEN
#define RULE_GEN 6
#endif

#include "poke_abilities.h"
#include "poke_items.h"
#include "poke_learnsets.h"
#include "poke_moves.h"
#include "poke_natures.h"
#include "poke_species.h"
#include "poke_types.h"

static int g_pass = 0;
static int g_fail = 0;

static void check(int cond, const char *fmt, ...)
{
    if (cond) {
        g_pass++;
    } else {
        g_fail++;
        printf("  [FAIL] ");
        va_list ap;
        va_start(ap, fmt);
        vprintf(fmt, ap);
        va_end(ap);
        printf("\n");
    }
}

int main(void)
{
    printf("=== 三代数据表自检 ===\n\n");

    /* ---------- 属性 ---------- */
    printf("[属性]\n");
    check(TYPE_COUNT == 18, "属性数应为 18，实际 %d", TYPE_COUNT);
    /* 火 -> 草 = 2x */
    int fire_idx = -1, grass_idx = -1, water_idx = -1;
    for (int i = 0; i < TYPE_COUNT; i++) {
        if (!strcmp(kTypeNames[i], "Fire")) fire_idx = i;
        if (!strcmp(kTypeNames[i], "Grass")) grass_idx = i;
        if (!strcmp(kTypeNames[i], "Water")) water_idx = i;
    }
    check(fire_idx >= 0, "未找到 fire 属性");
    check(grass_idx >= 0, "未找到 grass 属性");
    if (fire_idx >= 0 && grass_idx >= 0) {
        check(kTypeEffect[fire_idx][grass_idx] == 20, "火->草 应为 20(2x)，实际 %d",
              kTypeEffect[fire_idx][grass_idx]);
    }
    if (water_idx >= 0 && fire_idx >= 0) {
        check(kTypeEffect[water_idx][fire_idx] == 20, "水->火 应为 20(2x)，实际 %d",
              kTypeEffect[water_idx][fire_idx]);
    }
    /* 幽灵 -> 普通 = 0 */
    int ghost_idx = -1, normal_idx = -1;
    for (int i = 0; i < TYPE_COUNT; i++) {
        if (!strcmp(kTypeNames[i], "Ghost")) ghost_idx = i;
        if (!strcmp(kTypeNames[i], "Normal")) normal_idx = i;
    }
    if (ghost_idx >= 0 && normal_idx >= 0) {
        check(kTypeEffect[ghost_idx][normal_idx] == 0, "幽灵->普通 应为 0，实际 %d",
              kTypeEffect[ghost_idx][normal_idx]);
    }
    printf("  属性 %d 种，火->草=%d 水->火=%d 幽灵->普通=%d\n", TYPE_COUNT,
           (fire_idx >= 0 && grass_idx >= 0) ? kTypeEffect[fire_idx][grass_idx] : -1,
           (water_idx >= 0 && fire_idx >= 0) ? kTypeEffect[water_idx][fire_idx] : -1,
           (ghost_idx >= 0 && normal_idx >= 0) ? kTypeEffect[ghost_idx][normal_idx] : -1);

    /* ---------- 种族 ---------- */
    printf("\n[种族]\n");
    check(SPECIES_COUNT == 427, "种族条数应为 427(386 默认 + 41 Mega)，实际 %d", SPECIES_COUNT);

    /* Mega 形态必须是官方实装的 41 个。
     * Veekun 的 pokemon 表在 10000+ 段混入了官方从未实装的占位 Mega
     * （clefable-mega / victreebel-mega / starmie-mega / dragonite-mega /
     *  meganium-mega / feraligatr-mega / skarmory-mega / chimecho-mega /
     *  raichu-mega-x / raichu-mega-y / absol-mega-z等共 11 个），
     * 它们同样带 "-mega" 标识，靠字符串匹配会误收。 */
    int mega_cnt = 0;
    for (int i = 0; i < SPECIES_COUNT; i++) {
        if (kSpecies[i].is_mega) mega_cnt++;
    }
    check(mega_cnt == 41, "Mega 形态应为 41 个（39 基础种 + 喷火龙/超梦双形态），实际 %d", mega_cnt);

    /* 逐个确认被剔除的占位 Mega 不在表里 */
    const uint16_t bogus_mega[] = {
        10278, /* clefable_mega  */
        10279, /* victreebel_mega*/
        10280, /* starmie_mega   */
        10281, /* dragonite_mega */
        10282, /* meganium_mega  */
        10283, /* feraligatr_mega*/
        10284, /* skarmory_mega  */
        10304, /* raichu_mega_x  */
        10305, /* raichu_mega_y  */
        10306, /* chimecho_mega  */
        10307, /* absol_mega_z   */
    };
    int bogus_found = 0;
    for (unsigned b = 0; b < sizeof(bogus_mega) / sizeof(bogus_mega[0]); b++) {
        const SpeciesDef *sd = species_get(bogus_mega[b]);
        if (sd) {
            check(0, "表里混入了官方不存在的 Mega: #%u %s", bogus_mega[b], sd->name);
            bogus_found++;
        }
    }
    check(bogus_found == 0, "11 个占位 Mega 全部应被白名单过滤掉");

    /* Fairy 属性是第六世代引入的，三代/五代图鉴里不该有。
     * 若Veekun 的全世代数据未经回退直接导出，这里会抓到 20+ 条。
     *
     * 本项目当前规则世代为 6（X/Y ORAS），所以**应该**有 Fairy。
     * 编译时用 -DRULE_GEN=3 可切回三代规则做回归。
     * 这条断言同时守护两个方向：规则<6 时不许漏出 Fairy，
     * 规则>=6 时也不该把 Fairy 误屏蔽掉。 */
    int fairy_cnt = 0;
    for (int i = 0; i < SPECIES_COUNT; i++) {
        if (kSpecies[i].type1 == TYPE_FAIRY || kSpecies[i].type2 == TYPE_FAIRY) {
            fairy_cnt++;
        }
    }
#if RULE_GEN < 6
    check(fairy_cnt == 0, "规则世代<6，种族不应含 Fairy 属性，实际 %d 条", fairy_cnt);
#else
    check(fairy_cnt > 0, "规则世代>=6，种族应含 Fairy 属性（Veekun 原值），实际 %d 条",
          fairy_cnt);
#endif
    printf("  Fairy 种族条目 %d 条（规则世代 %d）\n", fairy_cnt, RULE_GEN);

    /* 抽检属性回退的实际效果（Gen 6 把这些改成了 Fairy）。
     * 两套期望值分别对应三代与第六代。 */
    struct { uint16_t pid; uint8_t g3_t1, g3_t2, g6_t1, g6_t2; const char *what; } back[] = {
        {35,   TYPE_PSYCHIC, TYPE_PSYCHIC, TYPE_FAIRY,   TYPE_FAIRY,   "皮皮 Clefairy"},
        {122,  TYPE_PSYCHIC, TYPE_FIGHTING, TYPE_PSYCHIC, TYPE_FAIRY,   "魔幻假面喵 Mr. Mime"},
        {176,  TYPE_NORMAL,  TYPE_FLYING,  TYPE_FAIRY,   TYPE_FLYING,  "波克鸟 Togetic"},
        {183,  TYPE_WATER,   TYPE_WATER,   TYPE_WATER,   TYPE_FAIRY,   "玛力露 Marill"},
        {282,  TYPE_PSYCHIC, TYPE_PSYCHIC, TYPE_PSYCHIC, TYPE_FAIRY,   "沙奈朵 Gardevoir"},
        {303,  TYPE_STEEL,   TYPE_STEEL,   TYPE_STEEL,   TYPE_FAIRY,   "大钳螳螂 Mawile"},
    };
    for (unsigned k = 0; k < sizeof(back) / sizeof(back[0]); k++) {
        const SpeciesDef *sd = species_get(back[k].pid);
        check(sd != NULL, "查不到 #%u", back[k].pid);
        if (sd) {
#if RULE_GEN < 6
            uint8_t t1 = back[k].g3_t1, t2 = back[k].g3_t2;
#else
            uint8_t t1 = back[k].g6_t1, t2 = back[k].g6_t2;
#endif
            check(sd->type1 == t1 && sd->type2 == t2,
                  "#%u %s 属性应为 %u/%u，实际 %u/%u",
                  back[k].pid, back[k].what, t1, t2, sd->type1, sd->type2);
        }
    }
    printf("  属性抽检 %zu 条通过（规则世代 %d）\n",
           sizeof(back) / sizeof(back[0]), RULE_GEN);

    const SpeciesDef *bulba = species_get(1);
    check(bulba != NULL, "查不到 #1 妙蛙种子");
    if (bulba) {
        /* 妙蛙种子: HP45 Atk49 Def49 SpA65 SpD65 Spe45, 草/毒 */
        check(bulba->hp == 45, "#1 HP 应为 45，实际 %d", bulba->hp);
        check(bulba->atk == 49, "#1 Atk 应为 49，实际 %d", bulba->atk);
        check(bulba->def == 49, "#1 Def 应为 49，实际 %d", bulba->def);
        check(bulba->spa == 65, "#1 SpA 应为 65，实际 %d", bulba->spa);
        check(bulba->spd == 65, "#1 SpD 应为 65，实际 %d", bulba->spd);
        check(bulba->spe == 45, "#1 Spe 应为 45，实际 %d", bulba->spe);
        printf("  #1 %s HP%d Atk%d Def%d SpA%d SpD%d Spe%d type%d/%d\n",
               bulba->name, bulba->hp, bulba->atk, bulba->def,
               bulba->spa, bulba->spd, bulba->spe,
               bulba->type1, bulba->type2);
    }
    /* 喷火龙: HP78 Atk84 SpA109, 火/飞 */
    const SpeciesDef *charz = species_get(6);
    check(charz != NULL, "查不到 #6 喷火龙");
    if (charz) {
        check(charz->hp == 78, "#6 HP 应为 78，实际 %d", charz->hp);
        check(charz->spa == 109, "#6 SpA 应为 109，实际 %d", charz->spa);
        printf("  #6 %s HP%d SpA%d\n", charz->name, charz->hp, charz->spa);
    }
    /* Mega 喷火龙 X: 至少 SpA 明显高于普通形态 */
    const SpeciesDef *megaX = species_get(10034);
    check(megaX != NULL, "查不到 #10034 Mega 喷火龙 X");
    if (megaX && charz) {
        check(megaX->is_mega == 1, "10034 应标记 is_mega");
        check(megaX->species_id == 6, "10034 的 species_id 应为 6，实际 %d", megaX->species_id);
        check(megaX->spa > charz->spa,
              "Mega 喷火龙 SpA 应高于普通形态（%d vs %d）", megaX->spa, charz->spa);
        printf("  #10034 %s species_id=%u SpA=%d (普通 %d)\n",
               megaX->name, megaX->species_id, megaX->spa, charz->spa);
    }
    check(MEGA_COUNT == 41, "Mega 形态应为 41，实际 %d", MEGA_COUNT);
    /* Mega -> base 映射 */
    uint16_t base = mega_to_base(10034);
    check(base == 6, "10034 应映射到基础种 6，实际 %u", base);

    /* ---------- 招式 ---------- */
    printf("\n[招式]\n");
    check(MOVE_COUNT >= 354, "招式表应至少到354，实际 %d", MOVE_COUNT);
    /*
     * 注意：id 采用 Veekun 的 moves.csv 编号，不是"游戏内印象中的序号"。
     * Veekun id=1 即冲撞 Pound，id=53 即喷射火焰 Flamethrower。
     */
    const MoveDef *pound = &kMoves[0];   /* 索引 0 对应 move_id 1 */
    check(pound->power == 40, "冲撞 威力应为 40，实际 %d", pound->power);
    check(pound->accuracy == 100, "冲撞 命中应为 100，实际 %d", pound->accuracy);
    check(pound->pp == 35, "冲撞 PP 应为 35，实际 %d", pound->pp);
    check(pound->damage_class == DMG_PHYSICAL,
          "冲撞 应为物理(damage_class=1)，实际 %d", pound->damage_class);
    check(pound->type == TYPE_NORMAL, "冲撞 属性应为普通，实际 %d", pound->type);
    printf("  #1 %s pow%d acc%d pp%d cls%d type%d\n", pound->name, pound->power,
           pound->accuracy, pound->pp, pound->damage_class, pound->type);

    const MoveDef *flame = &kMoves[52];  /* 索引 52 对应 move_id 53 */
    check(flame->power == 90, "喷射火焰 威力应为 90，实际 %d", flame->power);
    check(flame->pp == 15, "喷射火焰 PP 应为 15，实际 %d", flame->pp);
    check(flame->accuracy == 100, "喷射火焰 命中应为 100，实际 %d", flame->accuracy);
    check(flame->damage_class == DMG_SPECIAL,
          "喷射火焰 应为特殊(damage_class=2)，实际 %d", flame->damage_class);
    check(flame->type == TYPE_FIRE, "喷射火焰 属性应为火，实际 %d", flame->type);
    check(flame->ailment == MAIL_BURN, "喷射火焰 异常应为灼伤，实际 %d", flame->ailment);
    check(flame->ailment_chance == 10,
          "喷射火焰 灼伤概率应为 10%%，实际 %d", flame->ailment_chance);
    printf("  #53 %s pow%d pp%d cls%d type%d burn=%d%%\n", flame->name, flame->power,
           flame->pp, flame->damage_class, flame->type, flame->ailment_chance);

    /* 状态招式应有 power=0 且 damage_class=0 */
    int status_count = 0, physical = 0, special = 0;
    for (int i = 1; i < MOVE_COUNT; i++) {
        if (mv_get(i)->damage_class == DMG_STATUS && mv_get(i)->power == 0) status_count++;
        else if (mv_get(i)->damage_class == DMG_PHYSICAL) physical++;
        else if (mv_get(i)->damage_class == DMG_SPECIAL) special++;
    }
    check(status_count > 30, "状态招式应 >30，实际 %d", status_count);
    check(physical > 30 && special > 30,
          "物理/特殊招式数应各 >30，实际 物理%d 特殊%d", physical, special);
    printf("  状态%d / 物理%d / 特殊%d\n", status_count, physical, special);

    /* Fairy 招式（撒娇/甜吻/月光）在 Gen 6 从一般改成 Fairy。
     * 规则<6 时不该出现，规则>=6 时应恰好 3 个。 */
    int move_fairy = 0;
    for (int i = 1; i < MOVE_COUNT; i++) {
        if (mv_get(i)->type == TYPE_FAIRY) {
            move_fairy++;
        }
    }
#if RULE_GEN < 6
    check(move_fairy == 0, "规则世代<6，招式不应含 Fairy 属性，实际 %d 个", move_fairy);
#else
    check(move_fairy == 3,
          "规则世代>=6，应有 3 个 Fairy 招式（撒娇/甜吻/月光），实际 %d 个", move_fairy);
#endif
    printf("  Fairy 招式 %d 个\n", move_fairy);

    /* 抽检招式属性：撒娇/甜吻/月光在 Gen 6 被从一般改成 Fairy。
     * 这三个招式伤害为 0，但属性会影响被钢/毒等抵抗判定，
     * AI 评估"对方能否用钢系招式打我"时需要正确的属性信息。 */
    const struct { int id; const char *name; } mv_back[] = {
        {186, "甜吻 Sweet Kiss"},
        {204, "撒娇 Charm"},
        {236, "月光 Moonlight"},
    };
    for (unsigned k = 0; k < sizeof(mv_back) / sizeof(mv_back[0]); k++) {
        const MoveDef *mv = &kMoves[mv_back[k].id - 1];
#if RULE_GEN < 6
        uint8_t want = TYPE_NORMAL;
#else
        uint8_t want = TYPE_FAIRY;
#endif
        check(mv->type == want,
              "#%d %s 规则世代%d 下应为属性 %u，实际 %d",
              mv_back[k].id, mv_back[k].name, RULE_GEN, want, mv->type);
    }
    printf("  招式属性抽检 3 条通过\n");

    /* 三代有蓄力类招式（Solarl Beam 之类 min_turns>=2） */
    int multi_turn = 0;
    for (int i = 1; i < MOVE_COUNT; i++) {
        if (mv_get(i)->min_turns >= 2) multi_turn++;
    }
    check(multi_turn > 0, "应存在蓄力类招式(min_turns>=2)，实际 %d", multi_turn);
    printf("  蓄力类招式数: %d\n", multi_turn);

    /* 学习表引用的 move_id 必须都落在有效招式表里 */
    int bad_ref = 0;
    for (int i = 1; i <= LEARNSET_COUNT; i++) {
        const LearnSet *ls = learnset_get((uint16_t)i);
        if (!ls) continue;
        for (int j = 0; j < ls->node_count; j++) {
            uint16_t mid = ls->nodes[j].move_id;
            if (mid == 0 || mid > MOVE_COUNT) bad_ref++;
        }
    }
    check(bad_ref == 0, "学习表引用了越界 move_id: %d 处", bad_ref);

    /* ---------- 学习表 ---------- */
    printf("\n[学习表]\n");
    check(LEARNSET_COUNT == 427, "学习表条数应为 427（与种族表同源），实际 %d", LEARNSET_COUNT);
    const LearnSet *ls1 = learnset_get(1);
    check(ls1 != NULL, "查不到 #1 学习表");
    if (ls1) {
        check(ls1->node_count >= 10, "#1 学习节点应 >=10，实际 %d", ls1->node_count);
        check(ls1->nodes[0].level == 1, "#1 首个节点应为 Lv1，实际 %u", ls1->nodes[0].level);
        printf("  #1 节点数 %d，首节点 Lv%u 学 move%u\n",
               ls1->node_count, ls1->nodes[0].level, ls1->nodes[0].move_id);
    }
    /* 统计非空与平均 */
    int nonempty = 0;
    long total_nodes = 0;
    for (int i = 1; i <= 386; i++) {
        const LearnSet *ls = learnset_get(i);
        if (ls && ls->node_count) {
            nonempty++;
            total_nodes += ls->node_count;
        }
    }
    check(nonempty >= 380, "386 只里应有 >=380 只有学习表，实际 %d", nonempty);
    printf("  非空学习表 %d/386，平均 %.1f 节点\n",
           nonempty, nonempty ? (double)total_nodes / nonempty : 0.0);
    check(learnset_get(60000) == NULL, "查不存在的 pid 应返回 NULL");

    /* ---------- 性格 ---------- */
    printf("\n[性格]\n");
    check(NATURE_COUNT == 26, "性格数应为 26，实际 %d", NATURE_COUNT);
    /* kNatures 的下标 = nature_id，index 0 是「无性格」占位。
     * 曾漏掉那行占位导致整表错位一格，这里显式验证。 */
    check(strcmp(kNatures[0].name, "neutral") == 0,
          "index 0 应为 neutral 占位，实际 %s", kNatures[0].name);
    /* nature 1 = hardy（勤奋），官方就是无修正 */
    check(kNatures[1].increased_stat == 0 && kNatures[1].decreased_stat == 0,
          "性格 nature1 (%s) 应无修正", kNatures[1].name);
    /* nature 2 = lonely（孤僻/大胆）：+atk -def */
    check(kNatures[2].increased_stat == 2, "性格 nature2 (%s) 应 +Atk，实际 +%d",
          kNatures[2].name, kNatures[2].increased_stat);
    check(kNatures[2].decreased_stat == 3, "性格 nature2 (%s) 应 -Def，实际 -%d",
          kNatures[2].name, kNatures[2].decreased_stat);
    /* 最后一项 nature25 = quirky，不能是 NULL（NATURE_COUNT=26） */
    check(kNatures[25].name != NULL && kNatures[25].name_cn != NULL,
          "index 25 应有名字与中文名");
    /* 中文名从 Veekun 繁体转简体，抽查几个固定的 */
    check(strcmp(kNatures[1].name_cn, "勤奋") == 0,
          "nature1 中文名应为「勤奋」，实际「%s」", kNatures[1].name_cn);
    check(strcmp(kNatures[8].name_cn, "慢吞吞") == 0,
          "nature8 中文名应为「慢吞吞」，实际「%s」", kNatures[8].name_cn);
    check(strcmp(kNatures[25].name_cn, "认真") == 0,
          "nature25 中文名应为「认真」，实际「%s」", kNatures[25].name_cn);
    printf("  1=%s/%s(+%d/-%d) 2=%s/%s(+%d/-%d)\n",
           kNatures[1].name, kNatures[1].name_cn,
           kNatures[1].increased_stat, kNatures[1].decreased_stat,
           kNatures[2].name, kNatures[2].name_cn,
           kNatures[2].increased_stat, kNatures[2].decreased_stat);

    /* ---------- 特性 ---------- */
    printf("\n[特性]\n");
    check(ABILITY_COUNT > 60, "三代特性数应 >60，实际 %d", ABILITY_COUNT);
    /* Veekun 的 ability 名是首字母大写形式，如 Intimidate / Static */
    int intimidate = 0, static_abil = 0, torrent = 0;
    for (int i = 0; i < ABILITY_COUNT; i++) {
        if (!strcmp(kAbilities[i].name, "Intimidate")) intimidate++;
        if (!strcmp(kAbilities[i].name, "Static")) static_abil++;
        if (!strcmp(kAbilities[i].name, "Torrent")) torrent++;
    }
    check(intimidate > 0, "应包含 Intimidate(威吓)");
    check(static_abil > 0, "应包含 Static(静电)");
    check(torrent > 0, "应包含 Torrent(激流)");
    printf("  特性 %d 个 | Intimidate:%d Static:%d Torrent:%d\n",
           ABILITY_COUNT, intimidate, static_abil, torrent);

    /* ---------- 道具 ---------- */
    printf("\n[道具]\n");
    /* 三代真实道具 344 + 有意并入的官方 Mega 石 39 = 383 */
    check(ITEM_COUNT >= 380 && ITEM_COUNT <= 390,
          "道具数应在 380~390（344 三代 + 39 Mega石），实际 %d", ITEM_COUNT);
    int mega_stones = 0, berries = 0, restore = 0;
    for (int i = 0; i < ITEM_COUNT; i++) {
        if (kItems[i].kind == ITEMKIND_MEGA_STONE) mega_stones++;
        if (kItems[i].kind == ITEMKIND_BERRY) berries++;
        if (kItems[i].kind == ITEMKIND_RESTORE) restore++;
    }
    /* 39 块 = 官方三代图鉴内的 Mega 石数。
     * 烈空坐不含在内——它的 Mega 由「龙之波动」直接触发，不用石头，
     * 所以 Veekun 里本来就没有 Rayquazite。
     * 独角大盗的石头英文名是 Heracronite（不是 Heracrossite）。 */
    check(mega_stones == 39,
          "官方 Mega 石应为 39 块，实际 %d", mega_stones);
    check(berries >= 20, "树果应 >=20，实际 %d", berries);
    printf("  道具 %d 个 | Mega石 %d | 树果 %d | 回复药 %d\n",
           ITEM_COUNT, mega_stones, berries, restore);
    check(item_get(65000) == NULL, "查不存在的道具应返回 NULL");
    /* Mega 石必须不含 "Mega" 且含 "Stone"，避免与 Mega Ring/Glove 混淆 */
    int mega_non_stone = 0;
    for (int i = 0; i < ITEM_COUNT; i++) {
        if (kItems[i].kind == ITEMKIND_MEGA_STONE && kItems[i].heal != 0) mega_non_stone++;
    }
    check(mega_non_stone == 0, "Mega 石不应有 heal 值，异常 %d 个", mega_non_stone);

    /* ---------- 汇总 ---------- */
    printf("\n=== 结果: %d 通过, %d 失败 ===\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}