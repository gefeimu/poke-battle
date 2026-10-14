/*
 * 会话层：把界面状态机（poke_ui_state）接到对战引擎（poke_battle）。
 *
 * 定位
 * ----
 * 路线图第 3 项「把状态机接到引擎」就是这一层。它仍然**不碰像素、不碰
 * GPIO**：吃按键事件、吐一屏数据（BattleScreen），中间那一堆「按键 →
 * 引擎 → 界面」的耦合都收在这里，所以能在主机侧用按键序列完整跑完一场
 * 6v6 并断言每一处状态迁移。
 *
 * 为什么值得单独一层，而不是塞进 main.c
 * ------------------------------------
 * 回合流程里有两处非平凡的状态，塞进 main 就再也断言不了了：
 *
 *   1. **日志逐条播放**。引擎一个回合会一次写好几条日志（回合分隔、
 *      出招、伤害、倒下……），界面必须一条一条地放，放完才把控制权
 *      交回玩家。所以有一个「播放游标」，与「引擎写了多少条」分开记。
 *   2. **强制换人**。出战那只濒死时不能出招，必须先换人；而且**不能
 *      用 battle_step 换** —— 引擎在出战濒死时会拒绝推进回合（防呆
 *      护栏，见 poke_battle.c），必须直接调 battle_switch。
 *
 * 阶段（谁在收键）
 * --------------
 *   SES_PHASE_MAIN    主菜单 / 队伍配置：按键进状态机，自己导航
 *   SES_PHASE_LOG     日志播放中：**按键不进状态机**。OK 推进一条、
 *                     长按跳到末尾。这两个界面没有导航，键的意义由本层解释
 *   SES_PHASE_INPUT   等我方输入：招式菜单 / 弹层 / 认输确认 / 换人
 *   SES_PHASE_RESULT  结算：OK 回主菜单
 *
 * 一个回合长这样
 * ------------
 *   INPUT ──出招/换人──▶ 引擎算一回合 ──▶ LOG（逐条播）──▶ 需要换人？
 *     ▲                                                      │
 *     └──────────────────────────────────────────────────────┘
 *                       分出胜负则转 RESULT
 *
 * 关于「谁来装队」
 * --------------
 * 队伍配置界面（能改性格/努力值/道具的那套选择器）是路线图第 5 项，还没做。
 * 按那里的结论，**先用手写死的队伍跑通全流程更划算**，所以这里带了
 * ses_load_default_teams()。它顺手给双方各 6 只、每只 4 招，足够打完一整局。
 * 注意：队伍**列表**（386 只的图鉴）的绘制与导航已经做好并接在本层
 * （菜单绘制，路线图第 4 项），缺的只是「选中之后改写这只的属性」。
 *
 * 内存
 * ----
 * Session 里直接放了整个 Battle（含 24 条 × 96 字节的日志环形缓冲），
 * 一共约 4 KB。**要放进静态区**，别在栈上开（ESP32-C3 的 task 栈没这么大）。
 */
#ifndef POKE_SESSION_H
#define POKE_SESSION_H

#include <stdbool.h>
#include <stdint.h>

#include "poke_ai.h"
#include "poke_battle.h"
#include "poke_input.h"
#include "poke_ui.h"
#include "poke_ui_state.h"

/* ---------------------------------------------------------------- 阶段 */

typedef enum {
    SES_PHASE_MAIN = 0,   /* 还没开打：主菜单 / 队伍配置 */
    SES_PHASE_LOG,        /* 正在播放日志 */
    SES_PHASE_INPUT,      /* 等我方输入 */
    SES_PHASE_RESULT,     /* 结算 */
    SES_PHASE_COUNT,
} SesPhase;

/*
 * 日志折行后单行的字节上限。引擎单条日志本身就是 96 字节封顶
 * （BattleLog 的行宽），折成两行也不会超过它。
 */
#define SES_LOG_LINE_MAX 96

/* 队伍槽名（"Lv50"）的字节上限 */
#define SES_LV_TEXT_MAX 8

/*
 * 状态行左右两侧文字的字节上限。
 *
 * 右侧放的是状态机给的键位提示，**最长的一条是弹层/换人/队伍配置共用的
 * 「滚动   确定   长按返回」= 12 个汉字(36B) + 2 个间隔(6B) ... 即 36 字节**，
 * 加上结尾 0 需要 37。曾经写成 24：snprintf 会把提示拦腰截断，而且正好截在
 * 半个汉字上 —— 屏幕上是一个乱码方块，日志里则是一条「看起来只是短了」的
 * 断言失败。所以按最长提示留够，别按「常见的那条」留。
 */
#define SES_STATUS_TEXT_MAX 48

/* ---------------------------------------------------------------- 会话 */

typedef struct {
    /* ---- 三层：按键 → 状态机 → 引擎 ---- */
    Battle    b;
    UiState   ui;
    KeyInput  keys;
    AIConfig  ai;
    SesPhase  phase;

    /* 折行要量像素宽。可以不设（NULL）：那时整条日志当成一行不折。 */
    const UiFonts *fonts;

    /* ---- 日志播放游标 ----
     * 两个都是**绝对序号**（从开打算起的第几条），不是环形缓冲里的下标 ——
     * 日志超过 MAX_LOG 会被覆盖，用下标记游标会在覆盖发生的那一刻错位。
     *   log_next  第一条还没显示的日志
     *   log_end   本次播放的终点（不含）
     * 播完（log_next == log_end）才决定下一步：结算 / 强制换人 / 等输入。 */
    uint32_t  log_next;
    uint32_t  log_end;

    /* 本层自己消化掉一次长按之后，紧随其后的 CLICK 要吞掉。理由与
     * poke_ui_state 里那条一样：上游（官方按键组件）有时会 LONG 与
     * CLICK 都报，不挡就会「长按跳过日志」顺手把下面那一项也确定了。 */
    int8_t    swallow_key;

    /* 界面正停在「强制换人」上：下一个 SWITCH_MON 直接 battle_switch，
     * 不能走 battle_step（引擎在出战濒死时拒绝推进回合）。 */
    bool      force_pending;

    /* 招式菜单现在是「挣扎」而不是普通招式（所有招式 PP 都用光了）。
     * 见 poke_battle.h 的 STRUGGLE_MOVE_ID。 */
    bool      struggle_menu;

    /* 玩家已选的预设队序号（0..UI_PRESET_COUNT-1）。回主菜单时按它重装，
     * 所以「再来一局」保留玩家选的那支，而不是弹回默认队。 */
    uint8_t   team_index;

    /* 对手随机抽签用的会话级种子。**刻意不占用 battle_rng** ——
     * 在队伍装载阶段消耗引擎随机流会把整局的随机序列挪位，单测就不可重放了。
     * 真机上可由 main 用 esp_random() 播种。 */
    uint32_t  team_seed;

    /* ---- 展示字符串 ----
     * BattleScreen / MenuScreen 里全是 `const char *`，指向的内存必须活得比
     * 绘制调用长，所以缓存放在这里。ses_view() / ses_menu_view() 会刷新它们
     * （因此那两个函数接收的不是 const）。 */
    char      log_text[2][SES_LOG_LINE_MAX];  /* 对话框两行（已折行） */
    char      lv_text[2][SES_LV_TEXT_MAX];    /* [0] = 我方，[1] = 对手 */
    char      hp_text[2][16];                 /* "68/153" */
    char      pp_text[MOVES_PER_MON][8];      /* 招式菜单右上角的 PP */
    char      status_left[SES_STATUS_TEXT_MAX];
    char      status_right[SES_STATUS_TEXT_MAX];
    char      result_text[16];
    /*
     * 菜单用的三个字符串缓存。**尺寸按「%d 取到极值也放得下」留**，不按
     * 「现在这个数字有多长」留 —— 按现状留的话，将来 count 变成四位数
     * 就会被 snprintf 悄悄截断，而 -Wformat-truncation 正是为这个存在的。
     *   menu_left  "2147483647/2147483647" = 21 字节 + 结尾
     *   menu_sub   "我方剩余 2147483647 只" = 27 字节 + 结尾
     */
    char      menu_left[24];                  /* 菜单状态行左侧（"12/386"） */
    char      menu_sub[32];                   /* 结算大字下面那行 */
    char      menu_note[TEAM_SIZE][16];       /* 换人列表每行右侧的 "68/153" */

    /* ---- 统计（自检与调优用）---- */
    uint32_t  log_advances;   /* OK 推进日志的条数 */
    uint32_t  log_skips;      /* 长按快进到末尾的次数 */
} Session;

/* ---------------------------------------------------------------- 接口 */

/*
 * 初始化：装默认队伍、状态机复位到主菜单。
 * ai_level 是 AILevel（对手用），自检里用 AI_ROOKIE（不做搜索，快）。
 */
void ses_init(Session *s, uint8_t ai_level);

/* 装默认队伍（我方 = 已选的 team_index 对应的预设队，对手随机抽一支）。
 * ses_init 已经调过，重开一局不必再调。 */
void ses_load_default_teams(Session *s);

/*
 * 装第 idx 支预设队为**我方**，并为对手从五支里随机抽一支（idx 越界钳到 0）。
 * 顺带记下 team_index，供 ses_to_main 保留选择。
 */
void ses_load_preset(Session *s, int idx);

/* 折行用的字体。不设就整条日志画一行（自检里只关心文字内容时够用）。 */
void ses_set_fonts(Session *s, const UiFonts *fonts);

/* 开始一场：battle_start + 播放开场日志。队伍要先装好。 */
void ses_begin(Session *s);

/* 回主菜单（不重装队伍，可以再开一局） */
void ses_to_main(Session *s);

/* ---- 喂输入 ---- */

/*
 * 设备侧：每 1 ms 喂一次 ADC 读数。mv < 0 表示读失败（驱动会当未按下）。
 * 内部会取事件、投给会话，并顺势做连发。
 */
void ses_poll(Session *s, uint32_t now_ms, int mv);

/* 投一条按键事件。now_ms 必须单调递增（同 key_poll）。 */
void ses_feed(Session *s, const KeyMsg *msg, uint32_t now_ms);

/* 按住连发。设备侧每 16 ms 调一次；只在 MAIN / INPUT 阶段生效。 */
void ses_tick(Session *s, uint32_t now_ms);

/* ---- 出牌（供将来的 Mega 菜单调用）---- */

/*
 * 我方本回合出招前先超级进化。**不消耗回合**，必须在选招之前调用 ——
 * 这是引擎的约定（见 poke_battle.h 的 battle_mega）。mega_pid 从
 * kMegaMap 里取。
 *
 * 状态机里目前没有 Mega 菜单（弹层只有换人 / 认输），所以这一项目前
 * 没有调用方；先把引擎侧的接口钉住，菜单做好了直接接上。
 *
 * 返回 0 成功；-1 表示不在输入阶段、这只已经 Mega 过、或形态/石头不对。
 */
int ses_player_mega(Session *s, uint16_t mega_pid);

/* ---- 查询 ---- */

SesPhase       ses_phase(const Session *s);
const Battle  *ses_battle_c(const Session *s);
Battle        *ses_battle(Session *s);
const UiState *ses_ui_c(const Session *s);

/* 正在显示的日志绝对序号（INPUT 阶段显示的是最后播过的那条） */
uint32_t ses_log_index(const Session *s);

/* 单条日志的绝对序号换算成文字；越界（被环形缓冲覆盖了）返回 NULL */
const char *ses_log_at(const Session *s, uint32_t abs_index);

/*
 * 当前界面第 index 行的文字。
 * 主菜单 / 弹层 / 认输确认来自 ui_item_label（写死的文字）；
 * 队伍配置 / 换人列表来自队伍数据（种族名）—— 这一层替绘制层做映射，
 * 免得绘制层还要自己算「换人菜单第 3 项对应队伍里第几个槽」。
 */
const char *ses_row_text(const Session *s, int index);

/* 对话框里那一行（已折行）。idx = 0 / 1；没有文字时返回 NULL */
const char *ses_log_line(const Session *s, int index);

/* 结算文字："胜利" / "失败" / "平局"；未结算时返回 "" */
const char *ses_result_text(const Session *s);

/*
 * 把当前局面翻成绘制用的一屏。**会写 Session 里的字符串缓存**，
 * 所以参数不是 const —— 缓存必须活得比 ui_battle_screen() 长。
 */
void ses_view(Session *s, BattleScreen *out);

/* ---------------------------------------------------------------- 分层 */

/*
 * 调用方该画哪几层。
 *
 * 为什么要这个：弹层是**盖在战斗界面上**的，不是替换它 —— 对话框里的战况
 * 日志在弹层打开时仍然要看得见，玩家才知道自己在为什么做选择。而主菜单
 * 根本没有战斗界面可盖。所以「画什么」不是调用方猜的，是会话层按阶段
 * 与当前界面告诉它的。
 */
typedef enum {
    SES_LAYER_MENU = 0,     /* 只画菜单界面：主菜单 / 队伍配置 / 换人 / 结算 */
    SES_LAYER_BATTLE,       /* 只画战斗界面（招式菜单是它的一部分） */
    SES_LAYER_BATTLE_MENU,  /* 先画战斗界面，再在它上面盖一层弹层 */
} SesLayer;

SesLayer ses_layer(const Session *s);

/*
 * 当前界面翻成绘制用的一屏菜单。**返回 false 表示这一屏不该画菜单**
 * （战斗界面 / 日志播放中），此时 out 已被清零，可以安全忽略。
 *
 * 会写 Session 里的字符串缓存，所以参数不是 const。
 */
bool ses_menu_view(Session *s, MenuScreen *out);

#endif /* POKE_SESSION_H */
