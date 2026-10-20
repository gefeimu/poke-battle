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
 * 随机源约定
 * ----------
 *   **决策可重放**：每次 ai_choose 之前 rng_reset()，ai_choose 内部又自己
 *   save/restore，所以搜索不消耗对局的随机流（与真机一致）。
 *   **对局的随机性每回合换一个起点**：每回合开打前把随机源设成
 *   (game_seed, 回合号) 的确定性函数，见 arena_round_seed()。
 *   这一条是必须的：只把随机源重置成**同一个**种子，会让每回合抽到同一批数
 *   （暴击、伤害档、同速平局每回合一模一样）—— 纯镜像对局（双方队伍完全相同）
 *   于是被这套固定剧本决定胜负。实测（真镜像，N=60，理想 50%）：
 *   每决策重置 0x12345678 → side0 只赢 25%，换成 0xDEADBEEF → 43%；
 *   改成每回合换起点后，两个种子都落在 45~47%。整局仍完全可重放：
 *   同一个 game_seed → 同一场。
 *   随机源直接复用引擎的 s_rng（见 battle_rng_state），本文件不再自建一份
 *   —— 自建那份会让 ai_choose 的 save/restore 在测试里失效，并给 AI vs AI
 *   造出一个只由后手侧搜索长度决定的虚假 side0 劣势。
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

/* ---- 统一随机源：直接复用引擎的 s_rng（battle_rng_state）----
 *
 * 这里**不再自建 static s_rng**。自建的那份与 battle_rng_state() 指向的
 * 是引擎里另一个变量，于是：① ai_choose 的随机流 save/restore 在测试里
 * 形同虚设；② AI vs AI 时进入 battle_step 的随机流只由后动作一侧的搜索
 * 长度决定，凭空造出一个引擎里并不存在的 side0 劣势。合成一份后，测试与
 * 真机的随机行为一致。
 */
uint32_t battle_rng(void)
{
    uint32_t *st = battle_rng_state();
    *st ^= *st << 13;
    *st ^= *st >> 17;
    *st ^= *st << 5;
    return *st;
}

/* 伤害随机因子同源覆盖，保证 AI 的伤害预估可重放
 * （-DPOKE_CALC_TEST_NO_RAND 关掉了 poke_calc.c 内的实现）。*/
uint8_t calc_rand_85_100(void)
{
    uint32_t *st = battle_rng_state();
    *st ^= *st << 13;
    *st ^= *st >> 17;
    *st ^= *st << 5;
    return (uint8_t)(85 + (*st % 16));
}

static inline void rng_reset(void) { *battle_rng_state() = 0x12345678u; }

/*
 * 一局里**每回合**给引擎换一个随机起点。
 *
 * 只重置成同一个种子会让每回合抽到同一批数（暴击/伤害档/同速平局每回合
 * 一模一样）；纯镜像对局里这套固定剧本就成了胜负的唯一来源。这里由
 * (game_seed, 回合号) 确定性导出，所以「局内随机多样、整局完全可重放」两者兼得。
 *
 * **返回值不许是 0**：0 是 xorshift 的不动点，一旦落进去就永远抽 0。
 */
static inline uint32_t arena_round_seed(uint32_t game_seed, uint32_t turn)
{
    uint32_t v = game_seed * 2654435761u + turn * 0x9E3779B9u;
    return v ? v : 1u;
}

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

/* 询问 AI（决策可重放：先把随机源拨回固定起点，事后再还原调用方留下的流） */
static inline AIAction ask(Battle *b, int side, uint8_t level)
{
    AIConfig cfg;
    ai_config_default(&cfg, level);
    AIAction a;
    /*
     * 先记住调用方留给这一回合的随机起点，再拨回固定种子让搜索可重放；
     * ai_choose 返回后把起点还原回去。
     *
     * **必须还原**：不还原的话，这一回合的起点就被这次决策吃掉了 ——
     * 后面 random_action（随机手）和 battle_step 都只能从 0x12345678 开始抽，
     * 于是随机手永远选同一招、引擎每回合抽到同一批数（本文件开头记的那个坑）。
     */
    uint32_t keep = *battle_rng_state();
    rng_reset();
    ai_choose(b, side, &cfg, &a);
    *battle_rng_state() = keep;
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
 * game_seed 决定这一局的「引擎随机脚本」（见 arena_round_seed）：
 * 同一个 game_seed + 同一对队伍 → 完全同一场。
 * 返回 0 = side0 胜，1 = side1 胜，-1 = 平局/未终局。
 * bad_out 累加非法动作次数；turns_out 回写回合数（均可传 NULL）。
 */
static inline int arena_play(const ArenaTeam *t0, const ArenaTeam *t1,
                             uint8_t lv0, uint8_t lv1, uint32_t game_seed,
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
        /* 每回合换一个随机起点：局内随机多样（暴击/伤害档/同速平局每回合
         * 都不同），整局仍完全可重放。理由见文件头「随机源约定」。 */
        *battle_rng_state() = arena_round_seed(game_seed, b.turn);
        for (int side = 0; side < 2; side++) {
            const BattleMon *act = battle_active_c(&b, side);
            AIAction a;
            int ai = (lv[side] != ARENA_RANDOM);
            if (act && act->cur_hp == 0) {
                /* 濒死：必须换人（AI 或随机手各出一决策），不调 ai_apply */
                if (ai) {
                    a = ask(&b, side, lv[side]);
                } else {
                    random_action(&b, side, &a);   /* 从本回合起点抽签 */
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
                random_action(&b, side, &a);       /* 从本回合起点抽签 */
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
