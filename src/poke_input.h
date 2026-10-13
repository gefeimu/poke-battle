/*
 * 三键驱动：ADC 电压读数 → 键位 → 短按/长按事件。
 *
 * 为什么自己写，而不是直接用官方 BSP 的按键组件
 * -------------------------------------------
 * 官方 BSP 把 iot_button 包了一层，回调跑在共享的 esp_timer 任务里，
 * 主机侧跑不起来。本项目的约定是**所有逻辑都能在 PC 上跑断言**（见 README），
 * 输入层尤其需要 —— 去抖、长按这类时序问题在真机上只能靠手感判断。
 * 所以这里只依赖两样东西：一份 mV 读数、一个毫秒时基。
 * 设备侧每 1 ms 喂一次（与 BSP 的采样周期一致），主机侧测试直接喂序列。
 *
 * 数值全部来自官方仓，不是估的
 * ---------------------------
 * components/bsp/include/bsp_pins.h（电压窗口是半开区间，边界取相邻两档中点）：
 *
 *   { {0, 150}, {150, 447}, {447, 1900} }    上 / 下 / 确定
 *   BSP_BTN_MV_RELEASED_MIN = 1900           高于它即松开态（标称 3300，
 *                                            本机实测约 2959 —— 所以别照抄标称值）
 *   BSP_BTN_SHORT_PRESS_MS  = 180
 *   BSP_BTN_LONG_PRESS_MS   = 500
 *
 * docs/reference/shinku-chen/adc-ladder-keypad-long-press-misread.zh_CN.md
 *   「键位锁」是 fork 在上游的逐窗口判定之外新增的一层，不在上游 main 里：
 *     按下先确认 40 ms ／ 锁定期间忽略其它窗口 120 ms ／ 松手同样等 120 ms
 *
 * docs/reference/shinku-chen/three-key-input-event-semantics.zh_CN.md
 *   只有 CLICK 与 LONG 表示「用户下达了指令」；RELEASE 只表示键不再被按住，
 *   永远不当命令。由长按触发的动作必须忽略紧随其后的那次抬起。
 *
 * 两个必须记住的电气事实
 * ---------------------
 * 1. **ADC 读失败必须当「未按下」。** 若当成 0 mV，一次读错就是「上键被按住」，
 *    光标会自己往下跑。这里用 mv < 0 表示读失败。
 * 2. **三个键共用同一个 ADC 引脚**（GPIO0 / ADC1_CH0），靠分压电阻区分，
 *    所以「按住某个键」有可能被读成「另一个键」：按住时触点短暂断开，
 *    节点电压会朝松开态跑，途中依次穿过它上方每一个窗口。
 *    键位锁就是为这个加的 —— 症状是「短按一切正常，只有长按出问题」。
 */
#ifndef POKE_INPUT_H
#define POKE_INPUT_H

#include <stdbool.h>
#include <stdint.h>

/* ---------------------------------------------------------------- 参数 */

/* 电压窗口上界（mV），半开区间 [lo, hi)。与 bsp_button.c 的判定一致：
 * 落在所有窗口之外即为「未按下」。注意窗口宽度并不相同 —— 确定键那个窗口
 * 宽到 1453 mV，是为了和松开态（约 2959）拉开距离。 */
#define KEY_MV_UP_MAX     150
#define KEY_MV_DOWN_MAX   447
#define KEY_MV_OK_MAX     1900

#define KEY_PRESS_CONFIRM_MS   40   /* 窗口要连续稳定这么久才认作按下 */
#define KEY_LOCK_SWITCH_MS    120   /* 锁定期间换键、以及松手，都要等这么久 */
#define KEY_LONG_PRESS_MS     500   /* 长按触发时间 */

typedef enum {
    KEY_UP = 0,
    KEY_DOWN,
    KEY_OK,
    KEY_COUNT,
} KeyId;

typedef enum {
    KEY_EV_NONE = 0,
    KEY_EV_PRESS,    /* 按下（已过去抖确认） */
    KEY_EV_CLICK,    /* 短按完成：按下并抬起，且没触发长按 */
    KEY_EV_LONG,     /* 按住满 KEY_LONG_PRESS_MS */
    KEY_EV_RELEASE,  /* 抬起。**不是指令**，只用来结束「按住持续生效」的行为 */
} KeyEvent;

typedef struct {
    KeyId    key;
    KeyEvent ev;
    int      mv;     /* 判定这条事件所用的那次采样（-1 = 读失败）。排障用 */
    uint32_t t_ms;   /* 事件产生时刻 */
} KeyMsg;

/* 事件队列深度。一次 poll 最多产 2 条（CLICK + RELEASE），8 足够。
 * 满了就丢并计数 —— 宁可丢事件，也不能在驱动里阻塞。 */
#define KEY_QUEUE 8

typedef struct {
    /* ---- 采样与连续段 ---- */
    int      run_raw;      /* 当前连续段的分类结果，-1 = 不在任何窗口 */
    uint32_t run_at;       /* 当前连续段的起点时刻 */
    uint32_t samples;      /* 累计采样次数（自检用） */

    /* ---- 锁定 ---- */
    int8_t   locked;       /* 当前锁定的键；-1 = 未按下 */
    uint32_t press_at;     /* 按下时刻（长按计时起点） */
    bool     long_sent;    /* 本次按住是否已经报过长按 */

    /* ---- 事件队列 ---- */
    KeyMsg   q[KEY_QUEUE];
    uint8_t  q_head, q_tail, q_n;
    uint32_t dropped;      /* 因队列满而丢掉的事件数 */

    bool     inited;
} KeyInput;

/* ---------------------------------------------------------------- 接口 */

/* 复位。调用后按键视为未按下。 */
void key_init(KeyInput *in);

/*
 * 喂一次采样。
 *   now_ms  单调递增的毫秒时基。设备侧每 1 ms 调一次；**必须稳定**，
 *           采样间隔若抖动过大会让「连续稳定 N ms」的判据失准。
 *   mv      ADC 校准后的电压；**读失败传负数**（不要传 0）。
 *
 * 产生的 0~2 条事件进入内部队列，用 key_next() 取。
 */
void key_poll(KeyInput *in, uint32_t now_ms, int mv);

/* 取一条事件。返回 false 表示队列空。 */
bool key_next(KeyInput *in, KeyMsg *out);

/* 当前是否有键被按住 */
bool key_is_down(const KeyInput *in);

/* 当前按住的键；未按住返回 KEY_COUNT */
KeyId key_held(const KeyInput *in);

/* 把事件名/键名转成字符串，日志与测试用 */
const char *key_event_name(KeyEvent ev);
const char *key_name(KeyId key);

#endif /* POKE_INPUT_H */
