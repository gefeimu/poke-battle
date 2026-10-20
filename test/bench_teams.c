/*
 * bench_teams.c - 多队伍 AI 对战基准
 *
 * 目的：在**固定的、可重放**的种子序列下跑比 test_ai 里那 200 局大得多
 * 的样本，回答三个问题：
 *   1. 三个档位（ROOKIE/SMART/EXPERT）的强度到底差多少？
 *   2. 不同队伍风格（速攻 / 强攻 / 肉盾 / 随机）之间有没有系统性优劣？
 *      队伍因素和 AI 档位比起来，哪个影响更大？
 *   3. AI 对随机手的胜率在大样本下是否稳定（含平均回合数、平局率）。
 *
 * 为什么单独一个程序而不塞进 test_ai
 * ----------------------------------
 *   - test_ai 的断言只关心「是否显著优于基线」，200 局够用；这里要的是
 *     几十个格子 × 数百局的矩阵，跑完要几分钟，不适合塞进 make test。
 *   - 这里的输出是**给人看的统计量**（胜率矩阵 / 平均回合 / CSV），
 *     不是断言，所以不设通过线。
 *
 * 可重放
 * ------
 *   随机源约定见 ai_arena.h：决策用固定种子（可重放），**对局的随机性每回合
 *   换一个起点**（arena_round_seed(game_seed, 回合号)），game_seed 就是每局
 *   抽队伍用的那个种子。所以**一局的结果完全由双方队伍决定**，而局内的
 *   暴击/伤害档/同速平局是逐回合变化的。
 *   这就要求每局的两支队伍必须不同，否则 N 局会跑出 N 个逐字节相同的结果：
 *     - 档位阶梯 / 档位矩阵 / 大样本：每局用 random_team(种子) 现抽两支队伍；
 *     - 队伍风格：每个原型先取「按种族值指标排序的前 24 只」当池子，
 *       每局从池里不重复抽 6 只 —— 既保持原型特征，又有真正的样本差异。
 *   （**曾经**是「每次决策前把随机源重置成同一个种子」，于是每回合抽到同一批数，
 *   纯镜像格被固定剧本决定胜负；见 ai_arena.h 文件头的记录。）
 *
 * 队伍风格用**种族值**定义，数字全部来自 gen/poke_species.c，不手填：
 *     SPEED = 速度            OFFENSE = 攻击+特攻
 *     BULK  = HP+防御+特防     RANDOM  = 每局从 1..386 均匀抽 6 只
 *
 * 用法
 * ----
 *   make bench-teams
 *   ./build/bench_teams --quick             # 每格几十局的冒烟跑
 *   ./build/bench_teams --games 40 --csv out.csv
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "poke_ability.h"
#include "poke_item.h"
#include "poke_ai.h"
#include "poke_battle.h"
#include "poke_learnsets.h"
#include "poke_moves.h"
#include "poke_species.h"

#include "ai_arena.h"

/* ---------------------------------------------------------------- 统计 */

typedef struct {
    int games;
    int wins0;      /* side0 胜 */
    int draws;
    int turns_sum;
    int max_turns;
    int bad;        /* 非法动作累计 */
} Stat;

static void stat_add(Stat *s, int winner, int turns, int bad)
{
    s->games++;
    if (winner == 0) s->wins0++;
    else if (winner < 0) s->draws++;
    s->turns_sum += turns;
    if (turns > s->max_turns) s->max_turns = turns;
    s->bad += bad;
}

static double pct(int num, int den) { return den ? num * 100.0 / den : 0.0; }
static double avg_turns(const Stat *s) { return s->games ? (double)s->turns_sum / s->games : 0.0; }
static double now_s(void) { return (double)clock() / CLOCKS_PER_SEC; }

/* CSV 收集：每个格子一行 */
typedef struct {
    const char *suite;
    const char *a;
    const char *b;
    Stat st;
} Row;

#define MAX_ROWS 128
static Row g_rows[MAX_ROWS];
static int g_row_n = 0;

static Row *row_new(const char *suite, const char *a, const char *b)
{
    if (g_row_n >= MAX_ROWS) return &g_rows[MAX_ROWS - 1];
    Row *r = &g_rows[g_row_n++];
    r->suite = suite;
    r->a = a;
    r->b = b;
    memset(&r->st, 0, sizeof(r->st));
    return r;
}

/* ---------------------------------------------------------------- 队伍来源 */

/* 用一组种子现抽两支随机队伍（两支种子不同，否则就是镜像对局） */
static void draw_random_pair(uint32_t seed, ArenaTeam *a, ArenaTeam *b)
{
    MonSpec ms[TEAM_SIZE];
    random_team(ms, TEAM_SIZE, seed);
    for (int i = 0; i < TEAM_SIZE; i++) a->mon[i] = ms[i];
    random_team(ms, TEAM_SIZE, seed ^ 0x5DEECE66Du);
    for (int i = 0; i < TEAM_SIZE; i++) b->mon[i] = ms[i];
}

#define ARCH_POOL 24
#define ARCH_N 4

enum { ARCH_SPEED = 0, ARCH_OFFENSE, ARCH_BULK, ARCH_RANDOM };

static const char *kArchName[ARCH_N] = { "SPEED", "OFFENSE", "BULK", "RANDOM" };
static const char *kArchNameCN[ARCH_N] = { "速攻", "强攻", "肉盾", "随机" };

typedef struct {
    uint16_t pool[ARCH_POOL];
    int n;
} Pool;

static Pool g_pool[ARCH_N];

static int32_t arch_score(const SpeciesDef *s, int kind)
{
    switch (kind) {
    case ARCH_SPEED:   return s->spe;
    case ARCH_OFFENSE: return (int32_t)s->atk + s->spa;
    case ARCH_BULK:    return (int32_t)s->hp + s->def + s->spd;
    default:           return 0;
    }
}

/* 按指标降序（同分按 id 升序，保证可重放）排出前 ARCH_POOL 只 */
static void build_pool(int kind, Pool *out)
{
    typedef struct { uint16_t pid; int32_t score; } RowS;
    RowS rows[SPECIES_COUNT];
    int n = 0;
    for (int i = 0; i < SPECIES_COUNT; i++) {
        const SpeciesDef *s = &kSpecies[i];
        if (s->is_mega) continue;
        if (s->id < 1 || s->id > 386) continue;
        rows[n].pid = s->id;
        rows[n].score = arch_score(s, kind);
        n++;
    }
    int take = n < ARCH_POOL ? n : ARCH_POOL;
    for (int i = 0; i < take; i++) {
        int best = i;
        for (int j = i + 1; j < n; j++) {
            if (rows[j].score > rows[best].score ||
                (rows[j].score == rows[best].score && rows[j].pid < rows[best].pid)) {
                best = j;
            }
        }
        RowS t = rows[i]; rows[i] = rows[best]; rows[best] = t;
        out->pool[i] = rows[i].pid;
    }
    out->n = take;
}

/* 从池里不重复抽 6 只组队 */
static void arch_team(const Pool *p, uint32_t seed, ArenaTeam *t)
{
    uint16_t idx[ARCH_POOL];
    int n = p->n;
    for (int i = 0; i < n; i++) idx[i] = (uint16_t)i;
    for (int i = 0; i < TEAM_SIZE; i++) {
        uint16_t pid = 0;
        if (n > 0) {
            seed = seed * 1103515245u + 12345u;
            int pick = (int)((seed >> 16) % (unsigned)n);
            pid = p->pool[idx[pick]];
            idx[pick] = idx[n - 1];
            n--;
        }
        t->mon[i].pid = pid;
        t->mon[i].item = 0;
        t->mon[i].hp_pct = 0;
        pick_moves(pid, t->mon[i].mv);
    }
}

/* RANDOM 风格：每局从整个图鉴（1..386）均匀抽 6 只 */
static void random_arch_team(uint32_t seed, ArenaTeam *t)
{
    for (int i = 0; i < TEAM_SIZE; i++) {
        seed = seed * 1103515245u + 12345u;
        uint16_t pid = (uint16_t)(1 + (seed >> 16) % 386u);
        t->mon[i].pid = pid;
        t->mon[i].item = 0;
        t->mon[i].hp_pct = 0;
        pick_moves(pid, t->mon[i].mv);
    }
}

static void make_arch(int kind, uint32_t seed, ArenaTeam *t)
{
    if (kind == ARCH_RANDOM) random_arch_team(seed, t);
    else arch_team(&g_pool[kind], seed, t);
}

/* ---------------------------------------------------------------- 四套测试 */

static const char *kLevelName[3] = { "ROOKIE", "SMART", "EXPERT" };
static const uint8_t kLevel[3] = { AI_ROOKIE, AI_SMART, AI_EXPERT };

/* 套 1：档位阶梯 —— 各档打随机手，外加随机 vs 随机基线 */
static void suite_ladder(int N)
{
    printf("\n=== [套1] 档位阶梯：各档 vs 随机手（N=%d/档）===\n", N);

    Row *base = row_new("ladder", "RANDOM", "random");
    for (int g = 0; g < N; g++) {
        ArenaTeam a, b;
        draw_random_pair((uint32_t)(0x3000 + g * 7919), &a, &b);
        int turns = 0, bad = 0;
        int w = arena_play(&a, &b, ARENA_RANDOM, ARENA_RANDOM,
                           (uint32_t)(0x3000 + g * 7919), &turns, &bad);
        stat_add(&base->st, w, turns, bad);
    }
    printf("  %-9s %5.1f%%  (%d/%d)  平 %d  平均 %.1f 回合  非法 %d\n",
           "RANDOM(基线)", pct(base->st.wins0, base->st.games), base->st.wins0,
           base->st.games, base->st.draws, avg_turns(&base->st), base->st.bad);
    fflush(stdout);

    for (int lv = 0; lv < 3; lv++) {
        Row *r = row_new("ladder", kLevelName[lv], "random");
        for (int g = 0; g < N; g++) {
            ArenaTeam a, b;
            draw_random_pair((uint32_t)(0x3000 + g * 7919), &a, &b);
            int turns = 0, bad = 0;
            int w = arena_play(&a, &b, kLevel[lv], ARENA_RANDOM,
                               (uint32_t)(0x3000 + g * 7919), &turns, &bad);
            stat_add(&r->st, w, turns, bad);
        }
        printf("  %-9s %5.1f%%  (%d/%d)  平 %d  平均 %.1f 回合  非法 %d\n",
               kLevelName[lv], pct(r->st.wins0, r->st.games), r->st.wins0,
               r->st.games, r->st.draws, avg_turns(&r->st), r->st.bad);
        fflush(stdout);
    }
}

/* 套 2：档位对抗矩阵 —— 行=side0 档位，列=side1 档位，格内是 side0 胜率 */
static void suite_matrix(int N)
{
    printf("\n=== [套2] 档位对抗矩阵：side0 胜率 %%（N=%d/格；对角线≈50%% 说明无先手偏置）===\n", N);
    printf("            %8s %8s %8s\n", "ROOKIE", "SMART", "EXPERT");
    for (int i = 0; i < 3; i++) {
        printf("  %-8s ", kLevelName[i]);
        for (int j = 0; j < 3; j++) {
            Row *r = row_new("matrix", kLevelName[i], kLevelName[j]);
            for (int g = 0; g < N; g++) {
                ArenaTeam a, b;
                draw_random_pair((uint32_t)(0x4000 + g * 104729), &a, &b);
                int turns = 0, bad = 0;
                int w = arena_play(&a, &b, kLevel[i], kLevel[j],
                                   (uint32_t)(0x4000 + g * 104729),
                                   &turns, &bad);
                stat_add(&r->st, w, turns, bad);
            }
            printf(" %7.1f%%", pct(r->st.wins0, r->st.games));
            fflush(stdout);
        }
        printf("\n");
    }
}

/* 套 3：队伍风格对抗 —— 4 种风格互打（含对角线镜像对照） */
static void suite_archetype(int N)
{
    printf("\n=== [套3] 队伍风格对抗：side0 胜率 %%（N=%d/格；对角线是镜像对照）===\n", N);
    printf("            %8s %8s %8s %8s\n", "SPEED", "OFFENSE", "BULK", "RANDOM");
    for (int i = 0; i < ARCH_N; i++) {
        printf("  %-8s ", kArchName[i]);
        for (int j = 0; j < ARCH_N; j++) {
            Row *r = row_new("archetype", kArchName[i], kArchName[j]);
            for (int g = 0; g < N; g++) {
                uint32_t s0 = (uint32_t)(0x5000 + g * 31337 + i * 101);
                uint32_t s1 = (uint32_t)(0x5000 + g * 31337 + j * 977);
                ArenaTeam a, b;
                make_arch(i, s0, &a);
                make_arch(j, s1, &b);
                int turns = 0, bad = 0;
                int w = arena_play(&a, &b, AI_EXPERT, AI_EXPERT, s0, &turns, &bad);
                stat_add(&r->st, w, turns, bad);
            }
            printf(" %7.1f%%", pct(r->st.wins0, r->st.games));
            fflush(stdout);
        }
        printf("\n");
    }
}

/* 套 4：大样本稳定性 —— EXPERT vs 随机手 */
static void suite_stable(int N)
{
    printf("\n=== [套4] 大样本稳定性：EXPERT vs 随机手（N=%d）===\n", N);
    Row *r = row_new("stable", "EXPERT", "random");
    double t0 = (double)clock() / CLOCKS_PER_SEC;
    for (int g = 0; g < N; g++) {
        ArenaTeam a, b;
        draw_random_pair((uint32_t)(0x6000 + g * 2654435761u), &a, &b);
        int turns = 0, bad = 0;
        int w = arena_play(&a, &b, AI_EXPERT, ARENA_RANDOM,
                           (uint32_t)(0x6000 + g * 2654435761u), &turns, &bad);
        stat_add(&r->st, w, turns, bad);
        if ((g + 1) % 250 == 0) {
            printf("    ... %d/%d 局，当前胜率 %.1f%%\n", g + 1, N,
                   pct(r->st.wins0, r->st.games));
            fflush(stdout);
        }
    }
    double t1 = (double)clock() / CLOCKS_PER_SEC;
    printf("  %-9s %5.1f%%  (%d/%d)  平 %d  平均 %.1f 回合  最长 %d 回合  非法 %d\n",
           "EXPERT", pct(r->st.wins0, r->st.games), r->st.wins0, r->st.games,
           r->st.draws, avg_turns(&r->st), r->st.max_turns, r->st.bad);
    printf("  耗时 %.1fs\n", t1 - t0);
}

/* ---------------------------------------------------------------- CSV */

static void write_csv(const char *path)
{
    FILE *f = fopen(path, "w");
    if (!f) {
        printf("!! 打不开 CSV: %s\n", path);
        return;
    }
    fprintf(f, "suite,side0,side1,games,side0_wins,win_pct,draws,avg_turns,max_turns,illegal\n");
    for (int i = 0; i < g_row_n; i++) {
        const Row *r = &g_rows[i];
        fprintf(f, "%s,%s,%s,%d,%d,%.1f,%d,%.1f,%d,%d\n",
                r->suite, r->a, r->b, r->st.games, r->st.wins0,
                pct(r->st.wins0, r->st.games), r->st.draws,
                avg_turns(&r->st), r->st.max_turns, r->st.bad);
    }
    fclose(f);
    printf("\nCSV 已写入 %s（%d 行）\n", path, g_row_n);
}

/* ---------------------------------------------------------------- main */

int main(int argc, char **argv)
{
    int n_ladder = 500, n_matrix = 200, n_arch = 100, n_stable = 1000;
    const char *csv_path = NULL;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--csv") == 0 && i + 1 < argc) {
            csv_path = argv[++i];
        } else if (strcmp(argv[i], "--games") == 0 && i + 1 < argc) {
            int k = atoi(argv[++i]);
            if (k > 0) { n_ladder = n_matrix = n_arch = n_stable = k; }
        } else if (strcmp(argv[i], "--quick") == 0) {
            n_ladder = 80; n_matrix = 40; n_arch = 32; n_stable = 240;
        } else {
            printf("用法: bench_teams [--games N] [--quick] [--csv out.csv]\n");
            return 1;
        }
    }

    printf("=== 多队伍 AI 对战基准（PC, gcc -O2）===\n");
    printf("规模: 阶梯 %d/档 + 基线 %d | 档位矩阵 %d/格 × 9 | 风格矩阵 %d/格 × 16 | 大样本 %d\n",
           n_ladder, n_ladder, n_matrix, n_arch, n_stable);

    for (int k = ARCH_SPEED; k <= ARCH_BULK; k++) build_pool(k, &g_pool[k]);
    for (int k = 0; k < ARCH_N - 1; k++) {
        printf("%s池[", kArchNameCN[k]);
        for (int i = 0; i < g_pool[k].n; i++) {
            const SpeciesDef *s = species_get(g_pool[k].pool[i]);
            printf("%s%s", i ? " " : "", s ? s->name_cn : "?");
        }
        printf("] ");
    }
    printf("\n");
    fflush(stdout);

    double t0 = now_s(), ta;
    ta = now_s(); suite_ladder(n_ladder);    printf("  [套1 用时 %.1fs]\n", now_s() - ta); fflush(stdout);
    ta = now_s(); suite_matrix(n_matrix);    printf("  [套2 用时 %.1fs]\n", now_s() - ta); fflush(stdout);
    ta = now_s(); suite_archetype(n_arch);   printf("  [套3 用时 %.1fs]\n", now_s() - ta); fflush(stdout);
    ta = now_s(); suite_stable(n_stable);    printf("  [套4 用时 %.1fs]\n", now_s() - ta); fflush(stdout);
    double t1 = now_s();

    int total_bad = 0;
    for (int i = 0; i < g_row_n; i++) total_bad += g_rows[i].st.bad;
    printf("\n=== 全部完成，总耗时 %.1fs，非法动作合计 %d ===\n", t1 - t0, total_bad);

    if (csv_path) write_csv(csv_path);
    return 0;
}
