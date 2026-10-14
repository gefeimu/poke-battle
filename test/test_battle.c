/*
 * test_battle.c - 对战引擎单测
 *
 * 用固定随机源覆盖 battle_rng()，让每局可重放。
 * 断言的是"规则行为"而不是具体伤害值——伤害值由 test_calc.c 负责。
 */

#include <stdio.h>
#include <string.h>

#include "poke_ability.h"
#include "poke_item.h"
#include "poke_battle.h"

/* ---- 固定随机源 ---- */
static uint32_t s_rng = 0x12345678u;

uint32_t battle_rng(void)
{
    s_rng ^= s_rng << 13;
    s_rng ^= s_rng >> 17;
    s_rng ^= s_rng << 5;
    return s_rng;
}
static void rng_reset(void) { s_rng = 0x12345678u; }

/* 伤害随机因子同源覆盖（-DPOKE_CALC_TEST_NO_RAND 关掉了引擎内实现），
 * 不提供会链接失败。用同一个 s_rng 让伤害与命中抽签保持同一条流，
 * rng_reset 之后整个测试可重放。 */
uint8_t calc_rand_85_100(void)
{
    s_rng ^= s_rng << 13;
    s_rng ^= s_rng >> 17;
    s_rng ^= s_rng << 5;
    return (uint8_t)(85 + (s_rng % 16));
}

static int g_pass = 0, g_fail = 0;

static void ok(int cond, const char *what, const char *detail)
{
    if (cond) {
        g_pass++;
        printf("  [ OK ] %s\n", what);
    } else {
        g_fail++;
        printf("  [FAIL] %s  %s\n", what, detail ? detail : "");
    }
}

static void ok_u16(uint16_t got, uint16_t want, const char *what)
{
    char buf[80];
    snprintf(buf, sizeof(buf), "期望 %u 实际 %u", want, got);
    ok(got == want, what, buf);
}

/* ----  fixtures ---- */

/* 找第一个威利>0 的招式，避免硬编码 move id */
static uint16_t first_damaging_move(void)
{
    for (uint16_t i = 1; i < MOVE_COUNT; i++) {
        if (mv_get(i)->power > 0) {
            return i;
        }
    }
    return 0;
}

/*
 * 注意：双方各放一只替补。如果只有一只，一方全灭会直接判胜，
 * 就测不到"需要换人"的返回码了（check_battle_end 先于返回码计算）。
 */
static void setup_1v1(Battle *b, uint16_t a_pid, uint16_t d_pid)
{
    battle_init(b);
    b->me.name = "我方";
    b->foe.name = "对手";

    uint16_t mv[4] = {first_damaging_move(), 0, 0, 0};
    BattleMon m;
    mon_init(&m, a_pid, 50, 0, ABIL_NONE, 0, mv);
    battle_set_mon(b, 0, 0, &m);
    mon_init(&m, d_pid, 50, 0, ABIL_NONE, 0, mv);
    battle_set_mon(b, 0, 1, &m);
    battle_set_mon(b, 1, 0, &m);
    battle_set_mon(b, 1, 1, &m);
    battle_start(b);
}

int main(void)
{
    printf("=== 对战引擎单测 ===\n");

    /* ---------- 装配 ---------- */
    printf("\n[装配]\n");
    Battle b;
    setup_1v1(&b, 1, 4);
    const BattleMon *me = battle_active_c(&b, 0);
    const BattleMon *foe = battle_active_c(&b, 1);
    ok(me != NULL && foe != NULL, "双方都有出战宝可梦", "");
    ok_u16(me->cur_hp, me->hp_max, "初始 HP = 上限");
    ok(me->hp_max == calc_hp_max(&me->base), "hp_max 与 calc 一致", "");
    ok(me->alive, "初始为存活状态", "");
    ok(battle_has_alive(&b, 0) && battle_has_alive(&b, 1), "双方都有存活", "");

    /* ---------- 回合推进 ---------- */
    printf("\n[回合] 一回合后应有伤害日志\n");
    rng_reset();
    b.my_action = 0;
    b.foe_action = 0;
    int rc = battle_step(&b);
    ok(rc == 0, "回合正常结束", "");
    ok(b.turn == 2, "回合数递增", "");
    ok(me->pp[0] < me->pp_max[0], "PP 被消耗", "");
    ok(foe->cur_hp < foe->hp_max || !foe->alive, "对手受到了伤害", "");

    /* ---------- 濒死与换人 ---------- */
    printf("\n[濒死] 把对手打到 0 HP\n");
    setup_1v1(&b, 150, 1); /* 超梦 vs 妙蛙种子，确保能秒 */
    BattleMon *foe2 = battle_active(&b, 1);
    foe2->cur_hp = 1; /* 设置为 1，任何伤害都能打死 */
    rng_reset();
    b.my_action = 0;
    b.foe_action = 0;
    rc = battle_step(&b);
    ok(!foe2->alive, "对手濒死", "");
    ok((rc & 2) != 0, "返回需要对方换人", "");
    ok(b.need_switch_foe == 1, "need_switch_foe 置位", "");

    /* 对手换上第二只 */
    uint16_t mv2[4] = {first_damaging_move(), 0, 0, 0};
    BattleMon m2;
    mon_init(&m2, 4, 50, 0, ABIL_NONE, 0, mv2);
    battle_set_mon(&b, 1, 1, &m2);
    ok(battle_switch(&b, 1, 1) == 0, "对手换人成功", "");
    const BattleMon *foe3 = battle_active_c(&b, 1);
    ok(foe3->cur_hp == foe3->hp_max, "换上来的满血", "");
    ok(battle_active_c(&b, 1)->cur_hp > 0, "换人后出战者存活", "");

    /* 防呆护栏：漏换人时不得推进回合（否则整回合静默空转，
     * 战斗卡到回合上限判平局——基准测试曾 200 局全平） */
    printf("\n[防呆] 出战濒死未换人时拒绝推进\n");
    setup_1v1(&b, 150, 1);
    battle_active(&b, 0)->cur_hp = 0;
    uint16_t turn_before = b.turn;
    b.my_action = 0;
    b.foe_action = 0;
    int rc2 = battle_step(&b);
    ok(rc2 == 1, "返回需要我方换人", "");
    ok(b.turn == turn_before, "回合数不推进", "");
    ok(b.need_switch_me == 1, "need_switch_me 置位", "");

    /* ---------- 换人消耗回合 ---------- */
    printf("\n[换人] 换人不造成伤害\n");
    setup_1v1(&b, 6, 6);
    BattleMon *foe4 = battle_active(&b, 1);
    uint16_t hp_before = foe4->cur_hp;
    rng_reset();
    b.my_action = 0xFF;
    b.my_switch = 0; /* 已经是 0 号位，换自己不改变什么但应消耗回合 */
    b.foe_action = 0;
    battle_step(&b);
    ok(foe4->cur_hp == hp_before, "我方换人回合对手无伤害", "");

    /* ---------- 出手顺序 ---------- */
    printf("\n[顺序] 速度快的一方先出手\n");
    setup_1v1(&b, 6, 25); /* 喷火龙(速100) vs 皮卡丘(速90)，喷火龙先手 */
    /* 把双方都打到 1 HP 之外的水平，看谁先倒 */
    BattleMon *fast = battle_active(&b, 0);
    BattleMon *slow = battle_active(&b, 1);
    fast->cur_hp = 1;
    slow->cur_hp = 1;
    /* 喷火龙用电系招打电击怪应该 1x 左右，确保能秒 */
    rng_reset();
    b.my_action = 0;
    b.foe_action = 0;
    battle_step(&b);
    /* 喷火龙速度快，先出手，电击怪先倒 */
    ok(!slow->alive, "速度快的一方先出手（对手先倒）", "");
    ok(fast->alive, "先手方存活", "");

    /* ---------- PP 耗尽 ---------- */
    printf("\n[PP] PP 归零后无法出招\n");
    setup_1v1(&b, 6, 6);
    BattleMon *ppm = battle_active(&b, 0);
    ppm->pp[0] = 0;
    rng_reset();
    b.my_action = 0;
    b.foe_action = 0xFF; /* 对手也换人，避免干扰 */
    b.foe_switch = 0;
    battle_step(&b);
    ok(ppm->pp[0] == 0, "PP 不会变成负数", "");

    /* ---------- Mega 进化 ---------- */
    printf("\n[Mega] 进化改变种族值\n");
    setup_1v1(&b, 6, 6);
    BattleMon *mg = battle_active(&b, 0);
    uint16_t spa_before = calc_stat(&mg->base, 4);
    mg->base.item = 699; /* Charizardite X */
    ok(battle_mega(&b, 0, 10034) == 0, "Mega 喷火龙X 进化成功", "");
    uint16_t spa_after = calc_stat(&mg->base, 4);
    ok(spa_after > spa_before, "Mega 后特攻提高", "");
    ok(mg->active_pid == 10034, "active_pid 切换为 Mega", "");
    ok(battle_mega(&b, 0, 10034) == -1, "不能重复 Mega", "");
    /* 用错的 Mega 石（pid 不匹配） */
    setup_1v1(&b, 6, 6);
    battle_active(&b, 0)->base.item = 699;
    ok(battle_mega(&b, 0, 10033) == -1, "不匹配的 Mega 石被拒绝", "");
    /* 没带石头不能 Mega */
    setup_1v1(&b, 6, 6);
    ok(battle_mega(&b, 0, 10034) == -1, "未携带 Mega 石不能进化", "");
    /* 带错物种的石头不能 Mega */
    setup_1v1(&b, 6, 6);
    battle_active(&b, 0)->base.item = 698; /* Venusaurite */
    ok(battle_mega(&b, 0, 10034) == -1, "携带其他物种的 Mega 石不能进化", "");

    /* ---------- 能力变化 ---------- */
    printf("\n[效果] 剑舞提升自身攻击两级\n");
    setup_1v1(&b, 6, 6);
    BattleMon *sd_mon = battle_active(&b, 0);
    int atk_before = sd_mon->stage[1];
    /* 找剑舞的 move id：状态招 + 自身攻+2 */
    uint16_t swords = 0;
    for (uint16_t i = 1; i < MOVE_COUNT; i++) {
        const MoveDef *m = mv_get(i);
        if (m->category == MCAT_NET_GOOD_STATS && m->n_stat_changes == 1 &&
            m->stat_changes[0].stat == 2 && m->stat_changes[0].change == 2) {
            swords = i;
            break;
        }
    }
    ok(swords != 0, "找到剑舞类招式", "");
    if (swords) {
        sd_mon->move[0] = swords;
        sd_mon->pp[0] = 10;
        rng_reset();
        b.my_action = 0;
        b.foe_action = 0xFF; /* 对手换人，避免干扰 */
        b.foe_switch = 0;
        battle_step(&b);
        ok(sd_mon->stage[1] == atk_before + 2,
           "剑舞使攻击 +2", "");
        printf("       攻击等级 %d -> %d\n", atk_before, sd_mon->stage[1]);
    }

    /* ---------- 异常状态 ---------- */
    printf("\n[效果] 电磁波造成麻痹\n");
    setup_1v1(&b, 6, 6);
    BattleMon *tw_mon = battle_active(&b, 0);
    BattleMon *tw_def = battle_active(&b, 1);
    /* 电磁波：MCAT_AILMENT + MAIL_PARALYSIS，命中 90 */
    uint16_t twave = 0;
    for (uint16_t i = 1; i < MOVE_COUNT; i++) {
        if (mv_get(i)->category == MCAT_AILMENT &&
            mv_get(i)->ailment == MAIL_PARALYSIS) {
            twave = i;
            break;
        }
    }
    ok(twave != 0, "找到麻痹类招式", "");
    if (twave) {
        tw_mon->move[0] = twave;
        tw_mon->pp[0] = 20;
        rng_reset();
        b.my_action = 0;
        b.foe_action = 0xFF;
        b.foe_switch = 0;
        battle_step(&b);
        ok(tw_def->cond == CON_PARALYZE, "对手被麻痹", "");
        printf("       对手状态 = %d\n", tw_def->cond);
    }

    /* ---------- 多段攻击 ---------- */
    printf("\n[效果] 多段攻击一次行动造成多次伤害\n");
    setup_1v1(&b, 6, 6);
    BattleMon *mh_atk = battle_active(&b, 0);
    BattleMon *mh_def = battle_active(&b, 1);
    /* 找一个 2-5 段的招式 */
    uint16_t multi = 0;
    for (uint16_t i = 1; i < MOVE_COUNT; i++) {
        if (mv_get(i)->max_hits >= 2 && mv_get(i)->power > 0) {
            multi = i;
            break;
        }
    }
    ok(multi != 0, "找到多段招式", "");
    if (multi) {
        mh_atk->move[0] = multi;
        mh_atk->pp[0] = 20;
        rng_reset();
        b.my_action = 0;
        b.foe_action = 0xFF;
        b.foe_switch = 0;
        battle_step(&b);
        ok(mh_def->cur_hp < mh_def->hp_max, "多段攻击造成伤害", "");
        printf("       对手 HP %u/%u\n", mh_def->cur_hp, mh_def->hp_max);
    }

    /* ---------- 寄生种子 ---------- */
    printf("\n[效果] 寄生种子每回合吸血，草系免疫\n");
    setup_1v1(&b, 3, 1); /* 妙蛙花(草) vs 妙蛙种子(草)：应免疫 */
    BattleMon *seed_atk = battle_active(&b, 0);
    BattleMon *seed_def = battle_active(&b, 1);
    uint16_t leech = 0;
    for (uint16_t i = 1; i < MOVE_COUNT; i++) {
        if (mv_get(i)->ailment == MAIL_LEECH_SEED) {
            leech = i;
            break;
        }
    }
    ok(leech != 0, "找到寄生种子", "");
    if (leech) {
        seed_atk->move[0] = leech;
        seed_atk->pp[0] = 10;
        rng_reset();
        b.my_action = 0;
        b.foe_action = 0xFF;
        b.foe_switch = 0;
        battle_step(&b);
        ok(seed_def->leech_seeded == 0, "草系免疫寄生种子", "");
        printf("       对手 leech_seeded = %u\n", seed_def->leech_seeded);
    }

    /* ---------- 天气 ---------- */
    printf("\n[效果] 求雨招式改变天气\n");
    setup_1v1(&b, 6, 6);
    BattleMon *wx_atk = battle_active(&b, 0);
    uint16_t raindance = 0;
    for (uint16_t i = 1; i < MOVE_COUNT; i++) {
        if (mv_get(i)->type == TYPE_WATER && mv_get(i)->power == 0 &&
            (mv_get(i)->category == MCAT_WHOLE_FIELD || mv_get(i)->category == MCAT_FIELD)) {
            raindance = i;
            break;
        }
    }
    ok(raindance != 0, "找到求雨", "");
    if (raindance) {
        wx_atk->move[0] = raindance;
        wx_atk->pp[0] = 10;
        rng_reset();
        b.my_action = 0;
        b.foe_action = 0xFF;
        b.foe_switch = 0;
        battle_step(&b);
        ok(b.wx == WX_RAIN, "天气变为下雨", "");
        ok(b.wx_turns > 0, "天气有剩余回合", "");
        printf("       天气 = %d，剩余 %u 回合\n", b.wx, b.wx_turns);
    }

    /* ---------- 回复 ---------- */
    printf("\n[效果] 回复招式恢复 HP\n");
    setup_1v1(&b, 6, 6);
    BattleMon *hl_atk = battle_active(&b, 0);
    hl_atk->cur_hp = hl_atk->hp_max / 2;
    uint16_t recover = 0;
    for (uint16_t i = 1; i < MOVE_COUNT; i++) {
        if (mv_get(i)->category == MCAT_HEAL && mv_get(i)->heal > 0) {
            recover = i;
            break;
        }
    }
    ok(recover != 0, "找到回复招式", "");
    if (recover) {
        hl_atk->move[0] = recover;
        hl_atk->pp[0] = 10;
        rng_reset();
        b.my_action = 0;
        b.foe_action = 0xFF;
        b.foe_switch = 0;
        battle_step(&b);
        ok(hl_atk->cur_hp > hl_atk->hp_max / 2, "HP 得到回复", "");
        printf("       HP %u/%u\n", hl_atk->cur_hp, hl_atk->hp_max);
    }

    /* ---------- 特性：出场天气 ---------- */
    printf("\n[特性] 降雨特性改变天气\n");
    setup_1v1(&b, 6, 6);
    BattleMon *driz = battle_active(&b, 0);
    driz->base.ability = ABIL_DRIZZLE;
    /* 重新开场以触发出场特性 */
    battle_start(&b);
    ok(b.wx == WX_RAIN, "降雨特性使天气变为下雨", "");
    printf("       天气 = %d\n", b.wx);

    /* ---------- 特性：威吓 ---------- */
    printf("\n[特性] 威吓降低对方攻击\n");
    setup_1v1(&b, 6, 6);
    BattleMon *intim = battle_active(&b, 0);
    BattleMon *intim_foe = battle_active(&b, 1);
    intim->base.ability = ABIL_INTIMIDATE;
    int foe_atk_before = intim_foe->stage[1];
    battle_start(&b);
    ok(intim_foe->stage[1] == foe_atk_before - 1,
       "威吓使对方攻击 -1", "");
    printf("       对方攻击等级 %d -> %d\n", foe_atk_before, intim_foe->stage[1]);

    /* 威吓对能力下降免疫特性无效 */
    printf("\n[特性] 恒净之体免疫威吓\n");
    setup_1v1(&b, 6, 6);
    BattleMon *im2 = battle_active(&b, 0);
    BattleMon *im2f = battle_active(&b, 1);
    im2->base.ability = ABIL_INTIMIDATE;
    im2f->base.ability = ABIL_CLEAR_BODY;
    int cb_before = im2f->stage[1];
    battle_start(&b);
    ok(im2f->stage[1] == cb_before, "恒净之体不受威吓影响", "");
    printf("       对方攻击等级保持 %d\n", im2f->stage[1]);

    /* ---------- 特性：加速 ---------- */
    printf("\n[特性] 加速每回合提升速度\n");
    setup_1v1(&b, 6, 6);
    BattleMon *sb = battle_active(&b, 0);
    sb->base.ability = ABIL_SPEED_BOOST;
    int spe_before = sb->stage[5];
    rng_reset();
    b.my_action = 0;
    b.foe_action = 0xFF;
    b.foe_switch = 0;
    battle_step(&b);
    ok(sb->stage[5] == spe_before + 1, "回合末速度 +1", "");
    printf("       速度等级 %d -> %d\n", spe_before, sb->stage[5]);

    /* ---------- 特性：飘浮免疫地面 ---------- */
    printf("\n[特性] 飘浮免疫地面招式\n");
    setup_1v1(&b, 6, 6);
    BattleMon *lev_def = battle_active(&b, 1);
    lev_def->base.ability = ABIL_LEVITATE;
    /* 找一个地面系伤害招 */
    uint16_t ground_mv = 0;
    for (uint16_t i = 1; i < MOVE_COUNT; i++) {
        if (mv_get(i)->type == TYPE_GROUND && mv_get(i)->power > 0) {
            ground_mv = i;
            break;
        }
    }
    ok(ground_mv != 0, "找到地面招式", "");
    if (ground_mv) {
        BattleMon *ga = battle_active(&b, 0);
        ga->move[0] = ground_mv;
        ga->pp[0] = 20;
        uint16_t hp_b = lev_def->cur_hp;
        rng_reset();
        b.my_action = 0;
        b.foe_action = 0xFF;
        b.foe_switch = 0;
        battle_step(&b);
        ok(lev_def->cur_hp == hp_b, "飘浮使地面招式无效", "");
        printf("       对手 HP 保持 %u\n", lev_def->cur_hp);
    }

    /* ---------- 特性：大力士攻击翻倍 ---------- */
    printf("\n[特性] 大力士使攻击翻倍\n");
    ok(ability_atk_mult(ABIL_HUGE_POWER) == 200, "大力士倍率 = 200", "");
    ok(ability_atk_mult(ABIL_GUTS) == 150, "根性倍率 = 150", "");
    ok(ability_atk_mult(ABIL_INTIMIDATE) == 100, "威吓不影响攻击", "");

    /* ---------- 特性：查询函数 ---------- */
    printf("\n[特性] 查询函数\n");
    ok(ability_is_weather_setter(ABIL_DRIZZLE), "降雨是天气特性", "");
    ok(!ability_is_weather_setter(ABIL_INTIMIDATE), "威吓不是天气特性", "");
    ok(ability_weather(ABIL_DROUGHT) == WX_SUN, "干旱 = 晴天", "");
    ok(ability_weather(ABIL_SAND_STREAM) == WX_SAND, "扬沙 = 沙暴", "");
    ok(ability_immune_type(NULL, ABIL_VOLT_ABSORB, TYPE_ELECTRIC),
       "蓄电免疫电", "");
    ok(!ability_immune_type(NULL, ABIL_VOLT_ABSORB, TYPE_FIRE),
       "蓄电不免疫火", "");
    ok(ability_immune_ailment(NULL, ABIL_INSOMNIA, MAIL_SLEEP),
       "不眠免疫睡眠", "");
    ok(ability_immune_stat_drop(ABIL_WHITE_SMOKE), "白色烟雾免疫降级", "");
    ok(ability_immune_ohko(ABIL_STURDY), "结实抗一击必杀", "");
    ok(ability_speed_mult(NULL, ABIL_CHLOROPHYLL, WX_SUN) == 200,
       "叶绿素晴天加速", "");
    ok(ability_speed_mult(NULL, ABIL_CHLOROPHYLL, WX_NONE) == 100,
       "叶绿素无天气不加速", "");
    ok(ability_speed_mult(NULL, ABIL_SWIFT_SWIM, WX_RAIN) == 200,
       "悠游自如雨天加速", "");

    /* ---------- 特性表完整性 ---------- */
    printf("\n[特性] 数据表\n");
    ok_u16(ABILITY_COUNT, 251, "特性数应为 251（1-6 代）");
    int none_kind = 0;
    for (int i = 0; i < ABILITY_COUNT; i++) {
        if (kAbilities[i].kind == ABIL_NONE) {
            none_kind++;
        }
    }
    ok(none_kind == 0, "所有特性都应映射到真实枚举", "kind 全为 ABIL_NONE 说明映射断了");
    ok(ability_get(22) != NULL, "能查到 id=22 威吓", "");
    if (ability_get(22)) {
        ok(ability_get(22)->kind == ABIL_INTIMIDATE, "id 22 = 威吓", "");
    }
    ok(ability_get(9999) == NULL, "查不存在的特性返回 NULL", "");

    /* ---------- 道具：属性威力增强 ---------- */
    printf("\n[道具] 木炭增强火系招式威力\n");
    ok(item_type_boost(226, TYPE_FIRE) == 120, "木炭使火招 +20%", "");
    ok(item_type_boost(226, TYPE_WATER) == 100, "木炭不影响水招", "");
    ok(item_type_boost(0, TYPE_FIRE) == 100, "无道具不影响", "");
    ok(item_type_boost(218, TYPE_FIGHTING) == 120, "黑带使格斗 +20%", "");
    ok(item_type_boost(227, TYPE_DRAGON) == 120, "龙之牙使龙招 +20%", "");

    /* ---------- 道具：能力值增强 ---------- */
    /*
     * 加成已统一到 calc_stat（直接查 item_get 的数据表），
     * 不再是 poke_item.c 里无人调用的 item_stat_boost。
     * 所以这里直接验 calc_stat 的产物，避免「同一概念两处实现」再漂移。
     */
    printf("\n[道具] 粗骨头/电气球使能力翻倍\n");
    {
        Statsheet st;
        statsheet_init(&st, 143, 50, 0);   /* 卡比兽：攻110 防65 特攻65 特防110 */
        uint16_t atk0 = calc_stat(&st, 2);
        uint16_t def0 = calc_stat(&st, 3);
        uint16_t spa0 = calc_stat(&st, 4);
        uint16_t spd0 = calc_stat(&st, 5);

        st.item = 235; /* 粗骨头 */
        ok_u16(calc_stat(&st, 2), (uint16_t)(atk0 * 2), "粗骨头攻击翻倍");
        ok_u16(calc_stat(&st, 3), def0, "粗骨头不影响防御");

        st.item = 213; /* 电气球 */
        ok_u16(calc_stat(&st, 4), (uint16_t)(spa0 * 2), "电气球特攻翻倍");

        st.item = 204; /* 深海鳞片 */
        ok_u16(calc_stat(&st, 5), (uint16_t)(spd0 * 2), "深海鳞片特防翻倍");
    }

    /* ---------- 道具：会心 ---------- */
    printf("\n[道具] 焦点镜片提升会心率\n");
    ok(item_crit_stage(209) == 1, "焦点镜片 +1 暴击等级", "");
    ok(item_crit_stage(0) == 0, "无道具无加成", "");

    /* ---------- 道具：吃剩的东西 ---------- */
    printf("\n[道具] 吃剩的东西每回合回血\n");
    setup_1v1(&b, 6, 6);
    BattleMon *lo = battle_active(&b, 0);
    lo->base.item = 211; /* Leftovers */
    lo->cur_hp = lo->hp_max - 50;
    uint16_t lo_before = lo->cur_hp;
    rng_reset();
    b.my_action = 0;
    b.foe_action = 0xFF;
    b.foe_switch = 0;
    battle_step(&b);
    ok(lo->cur_hp > lo_before, "回合末回血", "");
    printf("       HP %u -> %u（上限 %u）\n", lo_before, lo->cur_hp, lo->hp_max);

    /* ---------- 道具：免疫查询 ---------- */
    printf("\n[道具] 免疫类道具\n");
    ok(item_immune_stat_drop(208), "气势头带防能力下降", "");
    ok(!item_immune_stat_drop(211), "吃剩的不防下降", "");
    ok(item_immune_flinch(190), "光亮粉末防畏缩", "");
    ok(!item_immune_flinch(211), "吃剩的不防畏缩", "");

    /* ---------- 道具：名字与查询 ---------- */
    printf("\n[道具] 数据表\n");
    ok(item_get(226) != NULL, "能查到 id=226 木炭", "");
    if (item_get(226)) {
        ok(strcmp(item_get(226)->name, "Charcoal") == 0, "id 226 = Charcoal", "");
    }
    ok(item_get(9999) == NULL, "查不存在的道具返回 NULL", "");
    ok(item_name(211) != NULL, "能取到道具名", "");
    printf("       id 211 = %s\n", item_name(211));

    /* 道具表完整性：不应再有 kind=HOLD 却带 boost_type 的错配 */
    int boosted = 0;
    for (int i = 0; i < ITEM_COUNT; i++) {
        if (kItems[i].boost_type != 0) {
            boosted++;
        }
    }
    ok_u16(boosted, 16, "应有 16 个属性增强道具（木炭/黑带/龙之牙等）");
    printf("       属性增强道具 %d 个：", boosted);
    for (int i = 0; i < ITEM_COUNT; i++) {
        if (kItems[i].boost_type != 0) {
            printf(" %s", kItems[i].name);
        }
    }
    printf("\n");

    /* ---------- 胜负判定 ---------- */
    printf("\n[胜负] 全队濒死判负\n");
    setup_1v1(&b, 6, 6);
    for (int i = 0; i < TEAM_SIZE; i++) {
        b.me.team[i].cur_hp = 0;
        b.me.team[i].alive = false;
    }
    ok(!battle_has_alive(&b, 0), "我方无存活", "");
    rng_reset();
    b.my_action = 0;
    b.foe_action = 0;
    battle_step(&b);
    ok(b.result == BATTLE_LOSE, "判负", "");

    printf("\n[索引] move_id 到招式表的映射必须正确\n");
    /*
     * 回归：曾经引擎用 kMoves[move_id] 取招式，而数组下标其实是
     * id-1，导致 280/353 个招式装配成「下一个招式」——喷火龙
     * 实际拿到 Mist（威力0、PP30）。它没崩溃、没越界（除了 id=354），
     * 只是数据 quietly 错了，而且与测试侧的错位互相抵消，所以长期
     * 没被发现。这里从「装配→读回」端到端钉住。
     */
    {
        int wrong = 0;
        for (uint16_t id = 1; id < MOVE_COUNT; id++) {
            uint16_t mv[4] = { id, 0, 0, 0 };
            BattleMon m;
            mon_init(&m, 1, 50, 0, ABIL_NONE, 0, mv);
            const MoveDef *want = mv_get(id);
            if (!want || m.pp_max[0] != (want->pp ? want->pp : 1)) {
                wrong++;
            }
        }
        char buf[80];
        snprintf(buf, sizeof(buf), "%d/%d 个 move id 装配错误", wrong,
                 MOVE_COUNT - 1);
        ok(wrong == 0, "全部 move id 的 PP 装配正确", buf);

        /*
         * 边界：最大合法 id 是 MOVE_COUNT（映射到 kMoves[MOVE_COUNT-1]），
         * 越界的是 MOVE_COUNT+1。以前我这里误把 MOVE_COUNT 当越界值，
         * 实际那是 Psycho_Boost 的合法 id。
         */
        uint16_t max_mv[4] = { MOVE_COUNT, 0, 0, 0 };
        BattleMon mm;
        mon_init(&mm, 1, 50, 0, ABIL_NONE, 0, max_mv);
        const MoveDef *maxdef = mv_get(MOVE_COUNT);
        ok(mm.move[0] == MOVE_COUNT && maxdef &&
           mm.pp_max[0] == (maxdef->pp ? maxdef->pp : 1),
           "最大合法 move_id 可用", "");

        uint16_t bad_mv[4] = { MOVE_COUNT + 1, 0, 0, 0 };
        BattleMon bm;
        mon_init(&bm, 1, 50, 0, ABIL_NONE, 0, bad_mv);
        ok(bm.move[0] == 0 && bm.pp_max[0] == 0,
           "越界 move_id 被拒绝（不读 kMoves[MOVE_COUNT]）", "");
    }

    printf("\n[边界] IV/EV 的 stat_idx 是 1-based\n");
    {
        /* 0 会让 iv[-1] 写到 base.mega，静默破坏 Mega 还原 */
        BattleMon m;
        uint16_t mv[4] = { 0, 0, 0, 0 };
        mon_init(&m, 6, 50, 0, ABIL_NONE, 0, mv);
        uint8_t mega_before = m.base.mega;
        mon_set_iv(&m, 0, 31);
        ok(m.base.mega == mega_before, "mon_set_iv(0) 不越界写", "");

        Statsheet sh;
        statsheet_init(&sh, 6, 50, 0);
        uint8_t iv5_before = sh.ey.iv[5];
        statsheet_set_ev(&sh, 0, 200);
        ok(sh.ey.iv[5] == iv5_before, "statsheet_set_ev(0) 不越界写", "");
    }

    printf("\n[日志] 超过 255 条后仍可读（count 不得回绕）\n");
    {
        /*
         * count 曾是 uint8_t，第 256 条回绕到 0，
         * log_get 的 `i >= count` 随即把整个日志区判空——
         * 用户视角是「打到一半战斗记录全没了」。
         */
        BattleLog lg;
        log_init(&lg);
        for (int i = 0; i < 300; i++) {
            log_add(&lg, "第%d条日志", i);
        }
        ok(lg.count == 300, "300 条后 count 仍正确", "");
        int readable = 0;
        for (uint16_t i = 0; i < MAX_LOG; i++) {
            if (log_get(&lg, i)) {
                readable++;
            }
        }
        char buf[80];
        snprintf(buf, sizeof(buf), "可读 %d/%d 条（环形覆盖后应满）",
                 readable, MAX_LOG);
        ok(readable == MAX_LOG, "环形覆盖后日志仍可读", buf);
    }

    printf("\n[天气] 持续 5 回合\n");
    {
        /*
         * wx_turns 递减在回合末，设 5 会被当回合减掉一次 -> 只 4 回合。
         * 注意必须处理 battle_step 的换人信号（1/2/3），否则濒死后
         * 回合不再推进、天气冻结，天真地看起来「永不结束」。
         */
        setup_1v1(&b, 3, 65);
        b.wx = WX_RAIN;
        b.wx_turns = 6; /* 与引擎一致：设 6 缓冲回合末那次递减 */
        int rain_turns = 0;
        for (int t = 0; t < 12; t++) {
            /* 先把濒死的换下去，保证每一步都能推进回合 */
            if (battle_active_c(&b, 0)->cur_hp == 0 &&
                battle_switch(&b, 0, 1) != 0) {
                break;
            }
            if (battle_active_c(&b, 1)->cur_hp == 0 &&
                battle_switch(&b, 1, 1) != 0) {
                break;
            }
            b.my_action = 0;
            b.foe_action = 0;
            battle_step(&b);
            if (b.wx == WX_RAIN) {
                rain_turns++;
            }
        }
        char buf[64];
        snprintf(buf, sizeof(buf), "实际生效 %d 回合", rain_turns);
        ok(rain_turns >= 5 && rain_turns <= 6, "雨天持续约 5 回合", buf);
    }

    printf("\n[状态] 换人不留僵死睡眠态\n");
    {
        /*
         * clear_volatile 曾只清 sleep_turns 不清 cond，留下
         * cond==CON_SLEEP 且 sleep_turns==0：行动分支要求 turns>0
         * 所以不成立（能行动），但 apply_ailment 每个分支都以
         * cond != CON_NONE 开头，于是再也挨不上任何异常。
         */
        setup_1v1(&b, 25, 28);
        /* 先给 0 号位放一只睡眠的，再换它下去 */
        BattleMon *leaving = &b.me.team[0];
        leaving->cond = CON_SLEEP;
        leaving->sleep_turns = 3;

        int rc = battle_switch(&b, 0, 1); /* 换 1 号位上场 */
        char buf[96];
        snprintf(buf, sizeof(buf),
                 "rc=%d leaving: cond=%d turns=%u（应同时清零）", rc,
                 leaving->cond, leaving->sleep_turns);
        ok(rc != 0 || (leaving->cond != CON_SLEEP && leaving->sleep_turns == 0),
           "被换下的睡眠宝可梦不留僵死态", buf);
    }

    printf("\n=== 结果: %d 通过, %d 失败 ===\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
