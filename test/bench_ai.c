/*
 * bench_ai.c - AI 决策耗时剖析
 *
 * 分别量三个档位单次决策的耗时，以及整场对局里 AI 累计占用的时间，
 * 用来核对 ESP32-C3 上的预算（PC 约 20~40 倍于 ESP32-C3）。
 */
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "poke_ability.h"
#include "poke_item.h"
#include "poke_ai.h"
#include "poke_battle.h"
#include "poke_learnsets.h"

static uint32_t s_rng = 0x12345678u;

uint32_t battle_rng(void)
{
    s_rng ^= s_rng << 13;
    s_rng ^= s_rng >> 17;
    s_rng ^= s_rng << 5;
    return s_rng;
}
uint8_t calc_rand_85_100(void)
{
    s_rng ^= s_rng << 13;
    s_rng ^= s_rng >> 17;
    s_rng ^= s_rng << 5;
    return (uint8_t)(85 + (s_rng % 16));
}
static void rng_reset(void) { s_rng = 0x12345678u; }

static uint16_t find_move(const char *name)
{
    for (uint16_t i = 1; i < MOVE_COUNT; i++) {
        if (strcmp(mv_get(i)->name, name) == 0) {
            return i;
        }
    }
    return 0;
}

typedef struct {
    uint16_t pid;
    const char *mv_name[4];
} MonSpec;

/* 静态表里只放名字，id 在 main 里解析成数字 */
static uint16_t g_ids[12][4];

/* 一支有 Mega 石头、有变化招、有回复招的队伍，覆盖各种候选类型 */
static const MonSpec TEAM_A[6] = {
    { 6,  { "Flamethrower", "Wing_Attack", "Slash", "Swords_Dance" } },
    { 150,{ "Hydro_Pump", "Surf", "Psychic", "Recover" } },
    { 212,{ "Metal_Claw", "Swords_Dance", "Slash", "Metal_Claw" } },
    { 3,  { "Sludge_Bomb", "Giga_Drain", "Light_Screen", "Synthesis" } },
    { 59, { "Thunderbolt", "Shadow_Ball", "Crunch", "Thunder_Wave" } },
    { 65, { "Leaf_Blade", "Synthesis", "Leaf_Blade", "Leaf_Blade" } },
};

static const MonSpec TEAM_B[6] = {
    { 9,  { "Blast_Burn", "Punch", "Swords_Dance", "Body_Slam" } },
    { 73, { "Thunderbolt", "Bubble_Beam", "Rock_Slide", "Barrier" } },
    { 130,{ "Hyper_Beam", "Waterfall", "Dragon_Dance", "Gust" } },
    { 94, { "Sludge_Bomb", "Flash_Cannon", "Explosion", "Self_Destruct" } },
    { 103,{ "Psychic", "Shadow_Ball", "Reflect", "Confuse_Ray" } },
    { 76, { "Megahorn", "Stomp", "Swords_Dance", "Facade" } },
};

static void resolve_ids(void)
{
    for (int t = 0; t < 12; t++) {
        const MonSpec *sp = (t < 6) ? &TEAM_A[t] : &TEAM_B[t - 6];
        for (int i = 0; i < 4; i++) {
            g_ids[t][i] = sp->mv_name[i] ? find_move(sp->mv_name[i]) : 0;
        }
    }
}

static void build(Battle *b)
{
    battle_init(b);
    b->me.name = "队伍A";
    b->foe.name = "队伍B";
    /* A队全员带对应 Mega 石，覆盖 Mega 候选分支 */
    for (int i = 0; i < 6; i++) {
        BattleMon m;
        mon_init(&m, TEAM_A[i].pid, 50, 0, ABIL_NONE, 0, g_ids[i]);
        battle_set_mon(b, 0, (uint8_t)i, &m);
    }
    for (int i = 0; i < 6; i++) {
        BattleMon m;
        mon_init(&m, TEAM_B[i].pid, 50, 0, ABIL_NONE, 0, g_ids[6 + i]);
        battle_set_mon(b, 1, (uint8_t)i, &m);
    }
    battle_start(b);

    /* 装配自检：hp_max 必须非零，否则战斗会永不终局 */
    for (int side = 0; side < 2; side++) {
        const BattleSide *s = side ? &b->foe : &b->me;
        for (int i = 0; i < 6; i++) {
            if (s->team[i].hp_max == 0) {
                printf("  !! 装配错误: %s 队 %u 号位 hp_max=0 (pid=%u)\n",
                       side ? "B" : "A", i, s->team[i].active_pid);
            }
        }
    }
}

static double now_s(void) { return (double)clock() / CLOCKS_PER_SEC; }

int main(void)
{
    resolve_ids();
    printf("=== AI 决策耗时剖析 (PC, gcc -O2) ===\n\n");

    /* ---- 1. 单次决策，各档位 ---- */
    const char *names[] = { "ROOKIE", "SMART", "EXPERT" };
    for (int lvl = 0; lvl < 3; lvl++) {
        AIConfig cfg;
        ai_config_default(&cfg, (uint8_t)lvl);
        const int N = 200;
        Battle b;
        build(&b);
        /* 先跑到第 3 回合，局面更有代表性 */
        for (int t = 0; t < 3; t++) {
            b.my_action = 0;
            b.foe_action = 0;
            battle_step(&b);
        }
        double t0 = now_s();
        for (int i = 0; i < N; i++) {
            Battle sim = b;
            AIAction a;
            rng_reset();
            ai_choose(&sim, 0, &cfg, &a);
        }
        double t1 = now_s();
        double ms = (t1 - t0) * 1000.0 / N;
        printf("  %-7s 节点上限=%3u 我的候选=%2u 对手应手=%2u MC=%2u"
               "  ->  %.3f ms/决策",
               names[lvl], cfg.max_nodes, cfg.my_cands, cfg.opp_replies,
               cfg.mc_playouts, ms);
        printf("   ESP32-C3 估 %.1f~%.1f ms\n", ms * 20, ms * 40);
        fflush(stdout);
    }

    /* ---- 2. 整场对局 ---- */
    printf("\n  整场 6v6 对局（AI_EXPERT 每回合决策）:\n");
    AIConfig cfg;
    ai_config_default(&cfg, AI_EXPERT);
    for (int g = 0; g < 3; g++) {
        Battle b;
        build(&b);
        rng_reset();
        int steps = 0;
        double t0 = now_s();
        while (b.result == BATTLE_ONGOING && b.turn < 200) {
            /* 双方都用 AI */
            AIAction am, af;
            rng_reset();
            ai_choose(&b, 0, &cfg, &am);
            rng_reset();
            ai_choose(&b, 1, &cfg, &af);
            ai_apply(&b, 0, &am);
            ai_apply(&b, 1, &af);
            int rc = battle_step(&b);
            steps++;
            /* 回合级进度：只在前 30 步打印，便于发现空转 */
            if (steps <= 30 && b.turn != (uint16_t)(steps / 2)) {
                printf("      回合%u rc=%d 我活%d 敌活%d\n", b.turn, rc,
                       battle_has_alive(&b, 0), battle_has_alive(&b, 1));
                fflush(stdout);
            }
            /* battle_step 的返回值就是「谁还需要换人」：
             *   1 = 只有我方，2 = 只有对方，3 = 双方都需要，0/-1 = 无需/终局。
             * 漏处理任何一个都会让双方出战位都判濒死而整回合空转，
             * 战斗会一直卡到回合上限。 */
            if (rc == 1 || rc == 3) {
                const BattleSide *s = &b.me;
                for (int i = 0; i < 6; i++) {
                    if ((uint8_t)i != s->active && s->team[i].cur_hp > 0) {
                        battle_switch(&b, 0, (uint8_t)i);
                        break;
                    }
                }
            }
            if (rc == 2 || rc == 3) {
                const BattleSide *s = &b.foe;
                for (int i = 0; i < 6; i++) {
                    if ((uint8_t)i != s->active && s->team[i].cur_hp > 0) {
                        battle_switch(&b, 1, (uint8_t)i);
                        break;
                    }
                }
            }
        }
        double t1 = now_s();
        printf("    第 %d 局: %2d 回合 / %3d 步, 胜者 %d, 耗时 %.2fs "
               "(每步 %.1f ms)\n",
               g + 1, b.turn, steps, b.result, t1 - t0,
               (t1 - t0) * 1000.0 / (steps ? steps : 1));
        fflush(stdout);
    }
    return 0;
}