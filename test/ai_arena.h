/*
 * ai_arena.h - AI 对战「竞技场」脚手架（test_ai 与 bench_teams 共用）
 *
 * 为什么是头文件而不是 .c
 * ----------------------
 * 这里要覆盖 battle_rng / calc_rand_85_100 两个**外部符号**（测试用
 * -DPOKE_BATTLE_TEST_NO_RNG -DPOKE_CALC_TEST_NO_RAND 关掉引擎内的实现）。
 * 符号覆盖要求它们与被测源码处在**同一个编译单元**，所以只能做成头文件；
 * 做成 .c 会引入独立链接单元，覆盖就不成立了。
 *
 * 为什么全是 static inline
 * ----------------------
 * 不同程序用到的子集不同（bench_teams 不用 act_str/slot_of），
 * static inline 不会因为「定义了但没用到」触发 -Wunused-function，
 * 编译仍能保持零警告。
 *
 * 随机源约定（与既有 951 项断言、README 里的胜率数字绑定，别改）
 * ------------------------------------------------------------
 *   每次 ai_choose / random_action 之前 rng_reset()，使**决策**可重放；
 *   引擎在 battle_step 里消耗的抽签，从重置后的位置继续。
 * 一旦改动这里，test_ai 的历史结论与 README 的胜率就再也对不上了。
 *
 * 别改 arena_play 里的换人处理
 * --------------------------
 * battle_step 的返回值 rc 表示「谁还需要换人」（1=side0 / 2=side1 / 3=双方）。
 * 漏处理任何一侧，双方出战位都会判濒死而整回合空转，战斗一直拖到回合上限
 * —— 这个坑在 test_ai 的注释里记着，是实测踩出来的。所以本文件是
 * play_game 的**唯一实现**，test_ai 与 bench_teams 都调它，不许各写一份。
 */
#ifndef AI_ARENA_H
#define AI_ARENA_H

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "poke_ability.h"
#include "poke_ai.h"
#include "poke_battle.h"
#include "poke_learnsets.h"

/* 该侧由随机手控制（不是 AI 档位） */
#define ARENA_RANDOM 0xFFu

/* ---- 固定随机源 ---- */
static uint32_t s_rng = 0x12345678u;

uint32_t battle_rng(void)
{
    s_rng ^= s_rng << 13;
    s_rng ^= s_rng >> 17;
    s_rng ^= s_rng << 5;
    return s_rng;
}

/* 伤害随机因子同源覆盖，保证 AI 的伤害预估可重放
 * （-DPOKE_CALC_TEST_NO_RAND 关掉了 poke_calc.c 内的实现）。*/
uint8_t calc_rand_85_100(void)
{
    s_rng ^= s_rng << 13;
    s_rng ^= s_rng >> 17;
    s_rng ^= s_rng << 5;
    return (uint8_t)(85 + (s_rng % 16));
}

static inline void rng_reset(void) { s_rng = 0x12345678u; }

static inline uint16_t find_move(const char *name)
{
    for (uint16_t i = 1; i < MOVE_COUNT; i++) {
        if (strcmp(mv_get(i)->name, name) == 0) {
            return i;
        }
    }
    return 0;
}

/* ---- fixtures ---- */

typedef struct {
    uint16_t pid;
    uint16_t item;
    uint16_t mv[4];
    uint8_t  hp_pct; /* 0 = 满血 */
} MonSpec;

typedef struct {
    MonSpec mon[TEAM_SIZE];
} ArenaTeam;

/* 建一方：返回放了几只 */
static inline int build_side(Battle *b, int side_is_foe, const MonSpec *specs, int n)
{
    for (int i = 0; i < n; i++) {
        BattleMon m;
        mon_init(&m, specs[i].pid, 50, 0, ABIL_NONE, specs[i].item, specs[i].mv);
        if (specs[i].hp_pct > 0) {
            m.cur_hp = (uint16_t)((uint32_t)m.hp_max * specs[i].hp_pct / 100);
            if (m.cur_hp == 0) {
                m.cur_hp = 1;
            }
        }
        battle_set_mon(b, side_is_foe, (uint8_t)i, &m);
    }
    return n;
}

static inline void start_battle(Battle *b, const MonSpec *mine, int nm,
                                const MonSpec *foes, int nf)
{
    battle_init(b);
    b->me.name = "我方";
    b->foe.name = "对手";
    build_side(b, 0, mine, nm);
    build_side(b, 1, foes, nf);
    battle_start(b);
}

/* 询问 AI（每次重置随机源保证可重放） */
static inline AIAction ask(Battle *b, int side, uint8_t level)
{
    AIConfig cfg;
    ai_config_default(&cfg, level);
    AIAction a;
    rng_reset();
    ai_choose(b, side, &cfg, &a);
    return a;
}

static inline const char *act_str(const AIAction *a, char *buf, size_t n)
{
    if (a->is_switch) {
        snprintf(buf, n, "换人到 %u 号位", a->slot);
    } else if (a->mega_pid) {
        snprintf(buf, n, "Mega %u 后用 %u 号招", a->mega_pid, a->slot);
    } else {
        snprintf(buf, n, "用 %u 号招", a->slot);
    }
    return buf;
}

/* 找某个招式在某只宝可梦招式槽里的下标，找不到返回 0xFF */
static inline uint8_t slot_of(const BattleMon *m, uint16_t move_id)
{
    for (int i = 0; i < 4; i++) {
        if (m->move[i] == move_id) {
            return (uint8_t)i;
        }
    }
    return 0xFF;
}

/* ---- 随机队伍（从 28 只的固定池里抓，招式按威力×命中挑前 4） ---- */

static inline void pick_moves(uint16_t pid, uint16_t out[4])
{
    const LearnSet *ls = learnset_get(pid);
    uint16_t cand[64];
    int n = 0;
    if (ls) {
        for (int k = 0; k < ls->node_count; k++) {
            if (ls->nodes[k].level > 50) continue;
            uint16_t mid = ls->nodes[k].move_id;
            if (mid == 0 || mid >= MOVE_COUNT) continue;
            int dup = 0;
            for (int j = 0; j < n; j++) if (cand[j] == mid) dup = 1;
            if (!dup && n < 64) cand[n++] = mid;
        }
    }
    for (int i = 0; i < 4; i++) {
        int best = -1;
        uint32_t best_score = 0;
        for (int j = 0; j < n; j++) {
            const MoveDef *md = mv_get(cand[j]);
            uint32_t sc = (uint32_t)md->power * (md->accuracy ? md->accuracy : 100);
            if (md->power == 0) sc /= 4;
            if (sc > best_score) { best_score = sc; best = j; }
        }
        if (best < 0) {
            out[i] = (i == 0) ? 33u : 0u;
        } else {
            out[i] = cand[best];
            cand[best] = cand[n - 1];
            n--;
        }
    }
}

static const uint16_t kPool[] = {
    3, 6, 9, 25, 59, 65, 68, 94, 112, 130, 143, 149, 154, 196, 212,
    230, 242, 248, 254, 282, 286, 306, 330, 334, 373, 376, 380, 384,
};
#define ARENA_POOL_N (sizeof(kPool) / sizeof(kPool[0]))

static inline void random_team(MonSpec *specs, int n, uint32_t seed)
{
    for (int i = 0; i < n; i++) {
        seed = seed * 1103515245u + 12345u;
        specs[i].pid = kPool[(seed >> 16) % ARENA_POOL_N];
        specs[i].item = 0;
        pick_moves(specs[i].pid, specs[i].mv);
        specs[i].hp_pct = 0;
    }
}

/* ---- 动作合法性 / 随机手 ---- */

static inline int action_valid(const Battle *b, int side, const AIAction *a)
{
    const BattleSide *us = side ? &b->foe : &b->me;
    const BattleMon *me = &us->team[us->active];
    if (a->mega_pid) {
        if (!ai_mon_can_mega(me)) return 0;
        if (mega_to_base(a->mega_pid) != me->active_pid) return 0;
    }
    if (a->is_switch) {
        if (a->slot >= TEAM_SIZE || a->slot == us->active) return 0;
        const BattleMon *t = &us->team[a->slot];
        return t->alive && t->cur_hp > 0;
    }
    if (a->slot >= MOVES_PER_MON) return 0;
    return me->move[a->slot] != 0 && me->pp[a->slot] > 0;
}

/* 随机手（用于强度对照）。没 PP 时会换人，什么都不行就空过 */
static inline void random_action(const Battle *b, int side, AIAction *a)
{
    const BattleSide *us = side ? &b->foe : &b->me;
    const BattleMon *me = &us->team[us->active];
    a->mega_pid = 0;
    a->is_switch = 0;
    a->slot = 0;

    /* 濒死必须换人。这条不能漏：之前随机手在濒死时仍然"出招"，
     * battle_step 就一直返回「对方需换人」而对手永远换不了，
     * 整局卡在同一个回合直到 guard 用尽（强度测试 16 局全判平局）。 */
    if (me->cur_hp == 0) {
        for (int i = 0; i < TEAM_SIZE; i++) {
            if ((uint8_t)i == us->active) continue;
            const BattleMon *t = &us->team[i];
            if (!t->alive || t->cur_hp == 0) continue;
            a->is_switch = 1;
            a->slot = (uint8_t)i;
            return;
        }
        return; /* 无替补，让引擎判负 */
    }

    int legal[4], n = 0;
    for (int i = 0; i < 4; i++) {
        if (me->move[i] && me->pp[i]) legal[n++] = i;
    }
    if (n > 0) {
        a->slot = (uint8_t)legal[battle_rng() % (unsigned)n];
        return;
    }
    /* PP 耗尽：换一只能打的上来 */
    for (int i = 0; i < TEAM_SIZE; i++) {
        if ((uint8_t)i == us->active) continue;
        const BattleMon *t = &us->team[i];
        if (!t->alive || t->cur_hp == 0) continue;
        int tl = 0;
        for (int k = 0; k < 4; k++) {
            if (t->move[k] && t->pp[k]) tl = 1;
        }
        if (tl) {
            a->is_switch = 1;
            a->slot = (uint8_t)i;
            return;
        }
    }
    /* 全军覆没：空过（引擎会记"什么都做不了"） */
}

/* 当前是否存在任何合法动作（用于区分"非法"和"无动作可用"） */
static inline int has_any_legal(const Battle *b, int side)
{
    const BattleSide *us = side ? &b->foe : &b->me;
    const BattleMon *me = &us->team[us->active];
    for (int i = 0; i < 4; i++) {
        if (me->move[i] && me->pp[i]) return 1;
    }
    for (int i = 0; i < TEAM_SIZE; i++) {
        if ((uint8_t)i == us->active) continue;
        const BattleMon *t = &us->team[i];
        if (!t->alive || t->cur_hp == 0) continue;
        for (int k = 0; k < 4; k++) {
            if (t->move[k] && t->pp[k]) return 1;
        }
    }
    return 0;
}

/*
 * 打一整局。lv0 / lv1 是该侧档位，取 ARENA_RANDOM 表示该侧用随机手。
 * 返回 0 = side0 胜，1 = side1 胜，-1 = 平局/未终局。
 * bad_out 累加非法动作次数；turns_out 回写回合数（均可传 NULL）。
 */
static inline int arena_play(const ArenaTeam *t0, const ArenaTeam *t1,
                             uint8_t lv0, uint8_t lv1,
                             int *turns_out, int *bad_out)
{
    uint8_t lv[2] = { lv0, lv1 };

    Battle b;
    battle_init(&b);
    b.me.name = "队伍0";
    b.foe.name = "队伍1";
    build_side(&b, 0, t0->mon, TEAM_SIZE);
    build_side(&b, 1, t1->mon, TEAM_SIZE);
    battle_start(&b);

    /* 每回合最多推进 600 步。正常一局 30~60 回合 × 每回合一次
     * battle_step + 最多两次换人，600 步有充足余量；上限存在是为了
     * 「引擎有 bug 导致 battle_step 不推进回合」时能停下来报错，
     * 而不是靠 100000 次空转把程序拖到超时。 */
    int guard = 0;
    int bad = 0; /* 本局非法动作累计，循环结束再回写，使 bad_out 可传 NULL */
    while (b.result == BATTLE_ONGOING && b.turn < 300 && guard < 600) {
        guard++;
        for (int side = 0; side < 2; side++) {
            const BattleMon *act = battle_active_c(&b, side);
            AIAction a;
            int ai = (lv[side] != ARENA_RANDOM);
            if (act && act->cur_hp == 0) {
                /* 濒死：必须换人（AI 或随机手各出一决策），不调 ai_apply */
                if (ai) {
                    a = ask(&b, side, lv[side]);
                } else {
                    rng_reset();
                    random_action(&b, side, &a);
                }
                if (!action_valid(&b, side, &a)) {
                    if (has_any_legal(&b, side)) bad++;
                }
                if (a.is_switch && battle_switch(&b, side, a.slot) != 0) {
                    bad++;
                }
                continue;
            }
            if (ai) {
                a = ask(&b, side, lv[side]);
            } else {
                rng_reset();
                random_action(&b, side, &a);
            }
            if (!action_valid(&b, side, &a)) {
                if (has_any_legal(&b, side)) bad++;
            }
            ai_apply(&b, side, &a);
        }
        /* rc 只是「谁还需要换人」的信号：1=side0 2=side1 3=双方。
         * 两侧的濒死已在上面的循环里处理过，这里 rc 通常是 0；
         * 若为 1/2/3 说明还有没换完的，下一轮循环会接着处理。 */
        int rc = battle_step(&b);
        if (rc < 0 && b.result == BATTLE_ONGOING) {
            /* battle_step 拒绝推进但战斗未结束 = 引擎空转，
             * 属于引擎缺陷，立即停下让断言暴露出来而不是空转到 guard 用尽。 */
            break;
        }
    }
    if (bad_out) *bad_out += bad;
    if (turns_out) *turns_out = b.turn;
    if (b.result == BATTLE_WIN) return 0;
    if (b.result == BATTLE_LOSE) return 1;
    return -1;
}

#endif /* AI_ARENA_H */
