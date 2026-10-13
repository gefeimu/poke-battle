/* bench_step.c - 测量 battle_step 成本，标定 AI 搜索预算 */
#include <stdio.h>
#include <time.h>

#include "poke_ability.h"
#include "poke_item.h"
#include "poke_battle.h"
#include "poke_learnsets.h"

static uint32_t s_rng = 0x9E3779B9u;
uint32_t battle_rng(void)
{
    s_rng ^= s_rng << 13;
    s_rng ^= s_rng >> 17;
    s_rng ^= s_rng << 5;
    return s_rng;
}

static double now_ms(void)
{
    return (double)clock() * 1000.0 / CLOCKS_PER_SEC;
}

/*
 * 从学习表里挑 4 个"最强"招式：按 威力*命中 排序。
 * 保证至少有一个威力招；纯状态表（不存在的）兜底用撞击。
 */
static void pick_moves(uint16_t pid, uint8_t level, uint16_t out[4])
{
    const LearnSet *ls = learnset_get(pid);
    uint16_t cand[MOVE_COUNT > 64 ? 64 : MOVE_COUNT];
    int n = 0;
    if (ls) {
        for (int k = 0; k < ls->node_count; k++) {
            if (ls->nodes[k].level > level) continue;
            uint16_t mid = ls->nodes[k].move_id;
            if (mid == 0 || mid >= MOVE_COUNT) continue;
            int dup = 0;
            for (int j = 0; j < n; j++) if (cand[j] == mid) dup = 1;
            if (!dup && n < 64) cand[n++] = mid;
        }
    }
    /* 选择排序取前 4（n 很小，O(n^2) 无压力） */
    for (int i = 0; i < 4; i++) {
        int best = -1;
        uint32_t best_score = 0;
        for (int j = 0; j < n; j++) {
            const MoveDef *md = mv_get(cand[j]);
            uint32_t sc = (uint32_t)md->power * (md->accuracy ? md->accuracy : 100);
            if (md->power == 0) sc /= 4; /* 状态招降权，但优于空槽 */
            if (sc > best_score) { best_score = sc; best = j; }
        }
        if (best < 0) {
            out[i] = (i == 0) ? 33u : 0u; /* Tackle 兜底 */
        } else {
            out[i] = cand[best];
            cand[best] = cand[n - 1];
            n--;
        }
    }
}

int main(void)
{
    Battle b;
    battle_init(&b);
    b.me.name = "我方";
    b.foe.name = "对手";

    uint16_t team_a[6] = {3, 6, 9, 65, 94, 282};
    uint16_t team_b[6] = {6, 9, 3, 94, 130, 282};
    uint16_t mv[4];
    BattleMon m;
    for (int i = 0; i < 6; i++) {
        pick_moves(team_a[i], 50, mv);
        mon_init(&m, team_a[i], 50, 0, ABIL_NONE, 0, mv);
        battle_set_mon(&b, 0, (uint8_t)i, &m);
        pick_moves(team_b[i], 50, mv);
        mon_init(&m, team_b[i], 50, 0, ABIL_NONE, 0, mv);
        battle_set_mon(&b, 1, (uint8_t)i, &m);
    }
    battle_start(&b);

    /* 统计一场自然对局的步数分布 */
    long total_steps = 0, total_turns = 0, wins = 0, losses = 0, draws = 0;
    const int ROUNDS = 200;
    double t0 = now_ms();
    for (int r = 0; r < ROUNDS; r++) {
        Battle sim = b;
        while (sim.result == BATTLE_ONGOING && sim.turn < 200) {
            sim.my_action = (uint8_t)(battle_rng() % 4);
            sim.foe_action = (uint8_t)(battle_rng() % 4);
            int rc = battle_step(&sim);
            total_steps++;
            /* 处理强制换人：随机挑一只没濒死的替补 */
            if (rc & 1) {
                for (int i = 0; i < TEAM_SIZE; i++) {
                    if (sim.me.team[i].alive && sim.me.team[i].cur_hp > 0) {
                        battle_switch(&sim, 0, (uint8_t)i);
                        break;
                    }
                }
            }
            if (rc & 2) {
                for (int i = 0; i < TEAM_SIZE; i++) {
                    if (sim.foe.team[i].alive && sim.foe.team[i].cur_hp > 0) {
                        battle_switch(&sim, 1, (uint8_t)i);
                        break;
                    }
                }
            }
        }
        total_turns += sim.turn;
        if (sim.result == BATTLE_WIN) wins++;
        else if (sim.result == BATTLE_LOSE) losses++;
        else draws++;
    }
    double t1 = now_ms();
    printf("随机出招: %d 局 (胜%ld/负%ld/平%ld), 平均 %.1f 回合/局, 共 %ld 步\n",
           ROUNDS, wins, losses, draws, (double)total_turns / ROUNDS, total_steps);
    printf("带日志: %ld 步 / %.1f ms => %.4f ms/步\n", total_steps, t1 - t0,
           (t1 - t0) / (double)total_steps);
    printf("(ESP32-C3 160MHz 约为 PC 的 1/20~1/40)\n");
    return 0;
}
