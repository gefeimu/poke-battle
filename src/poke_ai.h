/*
 * poke_ai.h - 对战 AI（三层决策）
 *
 * 设计目标：**不能太笨**。区分"笨 AI"与"像人"的分界不是算力
 * （ESP32-C3 上 5ms 能模拟上百个回合），而是会不会做这几件事：
 *   1. 选克制招而不是随便乱按
 *   2. 能打死人就抢先手打死，打不死就换人/强化，不做无意义动作
 *   3. 劣势会换人止损，优势不瞎换
 *   4. 安全时才强化，残血才回复
 *   5. 记得对手可能怎么应（2-ply 同时出手 minimax）
 *
 * 三层结构
 * --------
 * L1 启发式单层评估：对每个候选（招式/换人/Mega）打一个分。
 *    伤害用期望值（威力x命中x克制xSTABx能力等级），状态/强化/
 *    回复按局面价值给分。免疫与未实现的效果直接重罚。
 * L2 2-ply minimax：对 L1 排名前 N 的自身候选，枚举对手
 *    "最可能的前 K 种应手"，在克隆状态上真跑 battle_step，
 *    按对手让我最难受的情况给候选打分。同时出手、换人时机、
 *    先制抢血都在这一层体现。
 * L3 蒙特卡洛验证：对 L2 前几名的候选，从"我已行动"的局面
 *    开始让双方按 L1 启发式对打至终局，统计胜率，用来在
 *    分数接近时决胜（同时吸收命中率、连续命中、异常等方差）。
 *
 * 确定性
 * ------
 * 搜索消耗 battle_rng（模拟里的命中/暴击抽签）。测试里只要
 * 在每次 ai_choose 前把随机源 seed 成同一个值，决策就可重放。
 * 真机上 AI 会在搜索前后保存/恢复随机流状态，不让搜索
 * 消耗真实对局的熵。
 *
 * 预算
 * ----
 * 所有模拟都受 cfg->max_nodes（battle_step 次数）限制，
 * 超预算立即停并用当前最优。ROOKIE 档不做 L2/L3。
 */

#ifndef POKE_AI_H
#define POKE_AI_H

#include <stdbool.h>
#include <stdint.h>

#include "poke_battle.h"

/* ---------- 强度档位 ---------- */

typedef enum {
    AI_ROOKIE = 0, /* 只用 L1，带 15% 随机失手——给新手村用 */
    AI_SMART,      /* L1 + L2 + 6 局蒙特卡洛 */
    AI_EXPERT,     /* L1 + L2（更宽）+ 16 局蒙特卡洛 */
    AI_LEVEL_COUNT
} AILevel;

typedef struct {
    uint8_t  level;       /* AILevel */
    uint16_t max_nodes;   /* 模拟节点（battle_step）上限 */
    uint8_t  my_cands;    /* L2 考虑的自身候选数 */
    uint8_t  opp_replies; /* L2 考虑的对手应手数 */
    uint8_t  mc_playouts; /* L3 蒙特卡洛局数，0 = 关闭 */
} AIConfig;

/* ---------- 一个决策 ---------- */

typedef struct {
    uint8_t  is_switch;  /* 0 = 出招，1 = 换人 */
    uint8_t  slot;       /* 出招槽 0..3 / 换人目标 0..5 */
    uint16_t mega_pid;   /* 0 = 不超级进化（本回合出招前生效，不耗回合） */
} AIAction;

/* 按档位填默认配置 */
void ai_config_default(AIConfig *cfg, uint8_t level);

/* 档位中文名（UI 显示） */
const char *ai_level_name(uint8_t level);

/*
 * 决策。b 为只读（内部克隆后模拟），结果写入 out。
 * ai_is_foe：AI 控制的是哪一方（0 = me，1 = foe）。
 * 任何情况下都会给出合法动作；搜索超预算时退化为 L1 最优。
 */
void ai_choose(const Battle *b, int ai_is_foe, const AIConfig *cfg,
               AIAction *out);

/*
 * 把决策应用到战斗上（填 action/switch 字段，需要时先 Mega）。
 * 不推进回合，调用方随后自行调用 battle_step。
 */
void ai_apply(Battle *b, int side_is_foe, const AIAction *a);

/* 该方当前出战宝可梦是否具备 Mega 条件（有对应形态且带着正确的石头） */
bool ai_mon_can_mega(const BattleMon *m);

/* 局面静态评估（从 side 视角，越大越有利）。测试与调试用。 */
int32_t ai_eval_position(const Battle *b, int side);

#endif /* POKE_AI_H */
