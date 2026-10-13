/*
 * poke_battle.h - 第六代 6v6 对战引擎
 *
 * 设计约束
 * --------
 * - 纯 C11，不依赖动态内存。整个战斗状态是一个定长结构体，
 *   可以整体放进静态区或栈上，ESP32 上不用担心碎片。
 * - 本模块**不做 I/O**：不打印、不读按键、不画屏。
 *   它只推进状态机，通过 battle->log 输出事件文本，
 *   由 UI 层决定怎么显示。这样引擎可以在 PC 上单测，
 *   也可以直接跑在真机上。
 * - 随机源可替换：battle_rng() 是唯一的抽签入口，
 *   单测提供同名强符号覆盖即可固定结果。
 *
 * 已实现的规则（第六代）
 * ---------------------
 * - 6v6 完整对局，每只 4 个招式，PP 消耗
 * - 出手顺序：先制 > 优先级 > 速度（含麻痹减速）> 随机
 * - 伤害：gen6 公式（见 poke_calc.h），含能力等级、天气、暴击、STAB、克制、灼伤
 * - 换人：每回合一次，换人本身消耗回合
 * - 濒死判定与强制换人
 * - 速度/能力等级在场记忆，换人清空
 *
 * 有意简化（后续迭代）
 * -------------------
 * - 特性只做数据，未接入战斗逻辑
 * - 道具只做数据，未接入战斗逻辑
 * - 招式效果只做 EFFECT_NONE 与数据携带，具体效果分派是下一步
 * - 多段攻击、连续命中、蓄力、回复、吸收等按单段处理
 */

#ifndef POKE_BATTLE_H
#define POKE_BATTLE_H

#include <stdbool.h>
#include <stdint.h>

#include "poke_calc.h"
#include "poke_moves.h"

#define TEAM_SIZE 6
#define MOVES_PER_MON 4
#define MAX_LOG 24

/* 单场战斗的最大回合数，防止异常状态导致死循环 */
#define MAX_TURNS 300

/* ---------- 战斗中的一只宝可梦 ---------- */

typedef struct {
    Statsheet base;   /* 种族/等级/性格/努力值/道具/特性 */
    uint16_t cur_hp;
    uint16_t hp_max;

    /* 当前在场形态的 species_id。
     * Mega 进化时改成 Mega 的 pid，战斗结束后由 base.species_id 还原。
     * 与 base.species_id 分开存，是因为 Mega 只是形态切换，
     * 不应影响"这只宝可梦是谁"。 */
    uint16_t active_pid;

    /* 四个招式槽，0 = 空 */
    uint16_t move[4];
    uint8_t pp[4];
    uint8_t pp_max[4];

    Condition cond;
    int8_t stage[6];      /* 能力等级 -6..+6，索引 0..5 = hp/atk/def/spa/spd/spe */
    int8_t stage_acc;     /* 命中率等级，UI 显示用 */
    int8_t stage_eva;     /* 闪避率等级 */
    uint8_t sleep_turns;  /* 剩余睡眠回合 */
    uint8_t confused;     /* 剩余混乱回合 */
    uint8_t flinched;     /* 本回合是否畏缩（不可出招） */
    uint8_t leech_seeded; /* 被寄生种子（每回合末被吸） */
    uint8_t protect;      /* 本回合是否处于守住状态 */
    uint8_t mega_done;    /* 本场是否已 Mega 过（每只限一次） */
    bool alive;
} BattleMon;

/* ---------- 一方 ---------- */

typedef struct {
    BattleMon team[TEAM_SIZE];
    uint8_t active;        /* 当前出战下标，0..5 */
    uint8_t lead;          /* 首发下标 */
    const char *name;      /* "我方" / "对手" 等，仅用于日志 */
} BattleSide;

/* ---------- 回合事件日志 ---------- */
/*
 * 用固定长度字符串数组而不是 malloc。
 * 每条日志是一个完整句子，UI 层直接渲染。
 * 超出 MAX_LOG 时环形覆盖（旧条目被挤出）。
 *
 * 两个字段都必须是 16 位：
 *   - count 若用 uint8_t，第 256 条会回绕到 0，log_get 的
 *     `i >= count` 判断随即把整个日志区判空——用户视角是
 *     「打到一半战斗记录全没了」。MAX_TURNS=300 的战斗轻松
 *     产生 500+ 条日志。
 *   - 行宽 96 而非 48：日志全是中文，一个汉字 UTF-8 占 3 字节，
 *     48 字节只装 16 个字，且会在多字节字符中间截断产生乱码。
 */
typedef struct {
    char line[MAX_LOG][96];
    uint16_t head;  /* 下一条写入位置 */
    uint16_t count; /* 已写入总数（可能 > MAX_LOG） */
} BattleLog;

/* ---------- 一场战斗 ---------- */

typedef struct {
    BattleSide me;
    BattleSide foe;
    Weather wx;
    uint8_t wx_turns;      /* 天气剩余回合，0 = 无天气 */
    uint16_t turn;
    BattleLog log;

    /* 本回合双方的行动，由调用方填好后调 battle_step */
    uint8_t my_action;     /* 0..3 = 用第几个招式；0xFF = 换人 */
    uint8_t my_switch;     /* 换人目标下标 */
    uint8_t foe_action;    /* AI 填 */
    uint8_t foe_switch;

    /* 结果 */
    uint8_t result;        /* BATTLE_ONGOING / BATTLE_WIN / BATTLE_LOSE / BATTLE_DRAW */
    uint8_t need_switch_me;   /* 我方是否必须换人（上场那只濒死） */
    uint8_t need_switch_foe;
} Battle;

typedef enum {
    BATTLE_ONGOING = 0,
    BATTLE_WIN,
    BATTLE_LOSE,
    BATTLE_DRAW,
} BattleResult;

/* ---------- 日志 ---------- */

void log_init(BattleLog *l);
void log_add(BattleLog *l, const char *fmt, ...);
/* 取第 i 条日志（0 = 最早仍在缓冲区的），返回 NULL 表示越界 */
const char *log_get(const BattleLog *l, uint16_t i);

/* ---------- 队伍装配 ---------- */

/*
 * 初始化一只战斗宝可梦。
 *   species_id  pokemon id（1..386 或 Mega pid）
 *   level       等级
 *   nature      性格 0..24（25 种里 0 是 vacuous）
 *   ability     AbilityKind
 *   item        ItemDef.id，0 = 无
 *   moves       4 个 Veekun move id，0 表示该槽空
 */
void mon_init(BattleMon *m, uint16_t species_id, uint8_t level,
              uint8_t nature, uint8_t ability, uint16_t item,
              const uint16_t moves[4]);

/* 设置努力值（0..255）与 IV（0..31） */
void mon_set_ev(BattleMon *m, uint8_t stat_idx, uint8_t value);
void mon_set_iv(BattleMon *m, uint8_t stat_idx, uint8_t value);

/* ---------- 随机源 ---------- */

/*
 * 战斗内唯一抽签入口。默认实现是 xorshift32（见 poke_battle.c），
 * 单测可提供同名强符号覆盖以固定结果。
 * 返回 32 位随机数，调用方按需取模。
 */
uint32_t battle_rng(void);

/*
 * 随机源内部状态的读写入口（AI 搜索用）。
 * AI 在克隆状态上模拟几百个回合会大量消耗随机流；搜索前保存、
 * 搜索后恢复，真实对局的随机事件就不受搜索影响。
 * 单测覆盖 battle_rng 时本指针指向未被使用的内部状态，无害。
 */
uint32_t *battle_rng_state(void);

/* 日志静音开关（默认开）。AI 模拟时关闭可省掉大量 vsnprintf。 */
void battle_log_enable(bool on);
bool battle_log_enabled(void);

/* ---------- 战斗流程 ---------- */

/* 建一场空战斗，双方各 6 只空位 */
void battle_init(Battle *b);

/* 把装配好的宝可梦放进某一方。返回 0 成功，-1 失败（下标越界/已占用） */
int battle_set_mon(Battle *b, int side_is_foe, uint8_t slot, const BattleMon *m);

/* 双方都放好后调用：决定首发、输出开场日志、重置天气回合 */
void battle_start(Battle *b);

/*
 * 推进一个回合。调用方需先填好 b->my_action / b->my_switch。
 * 返回值：
 *   0  回合正常结束
 *   1  我方需换人（出战濒死）
 *   2  对方需换人
 *   3  双方都需换人
 * 负值表示参数错误。
 *
 * 换人消耗回合：只要任一方选择换人，该回合双方都只执行换人，
 * 不出招（与游戏一致）。
 */
int battle_step(Battle *b);

/* 换人。返回 0 成功，-1 下标无效或目标已濒死 */
int battle_switch(Battle *b, int side_is_foe, uint8_t slot);

/* Mega 进化（不消耗回合，需在本方出招前调用）。返回 0 成功 */
int battle_mega(Battle *b, int side_is_foe, uint16_t mega_pid);

/* 当前出战宝可梦（可能为 NULL） */
BattleMon *battle_active(Battle *b, int side_is_foe);
const BattleMon *battle_active_c(const Battle *b, int side_is_foe);

/* 该方是否还有能战斗的宝可梦 */
int battle_has_alive(const Battle *b, int side_is_foe);

/* 剩余 PP 总数，UI 显示用 */
uint16_t mon_pp_left(const BattleMon *m);

/* 是否处于"该招式无 PP 可用"状态 */
int mon_all_pp_gone(const BattleMon *m);

#endif /* POKE_BATTLE_H */
