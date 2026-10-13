/*
 * 三键驱动实现。设计与依据见 poke_input.h。
 */
#include "poke_input.h"

/* 未按下 / 不在任何窗口的哨兵。用 -1 而不是 KEY_COUNT，是为了让
 * 「读失败」和「松开区」共用同一条路径 —— 两者对外的语义都是「没有键按下」。 */
#define RAW_NONE (-1)

static int classify(int mv)
{
    /* 读失败：**必须当未按下**。若返回 KEY_UP，一次读错就等于「上键被按住」，
     * 光标会自己跑。BSP 的 button_level() 也是这么处理的（返回 INACTIVE）。 */
    if (mv < 0) {
        return RAW_NONE;
    }
    if (mv < KEY_MV_UP_MAX) {
        return KEY_UP;
    }
    if (mv < KEY_MV_DOWN_MAX) {
        return KEY_DOWN;
    }
    if (mv < KEY_MV_OK_MAX) {
        return KEY_OK;
    }
    return RAW_NONE;   /* 松开区（标称 3300，本机实测约 2959） */
}

static void push(KeyInput *in, KeyEvent ev, int key, int mv, uint32_t t)
{
    if (in->q_n >= KEY_QUEUE) {
        in->dropped++;
        return;
    }
    KeyMsg *m = &in->q[in->q_tail];
    m->key = (KeyId)key;
    m->ev = ev;
    m->mv = mv;
    m->t_ms = t;
    in->q_tail = (uint8_t)((in->q_tail + 1) % KEY_QUEUE);
    in->q_n++;
}

void key_init(KeyInput *in)
{
    if (!in) {
        return;
    }
    *in = (KeyInput){ 0 };
    in->run_raw = RAW_NONE;
    in->locked = RAW_NONE;
    in->inited = true;
}

void key_poll(KeyInput *in, uint32_t now_ms, int mv)
{
    if (!in) {
        return;
    }
    if (!in->inited) {
        key_init(in);   /* 防御：忘了初始化时按初始化处理，别拿垃圾值去判定 */
    }

    in->samples++;
    int raw = classify(mv);

    /* 连续段：值一变就重新起算。stable 就是「当前这个值已经连续保持了多久」。
     * 用 run_at 而不是「上次采样时刻」来算，这样中间只要没变化，
     * stable 会随 now_ms 自然增长。 */
    if (raw != in->run_raw) {
        in->run_raw = raw;
        in->run_at = now_ms;
    }
    uint32_t stable = now_ms - in->run_at;

    /* ---- 情况一：当前未按下，看是否该锁定一个键 ---- */
    if (in->locked == RAW_NONE) {
        if (raw != RAW_NONE && stable >= KEY_PRESS_CONFIRM_MS) {
            in->locked = (int8_t)raw;
            /*
             * 按下时刻取「电压进入这个窗口」的那一刻，而不是确认的那一刻。
             * 这样长按的 500 ms 是从真正接触开始算的，而不是从 +40 ms 起算；
             * 顺手也过滤掉按下瞬间电压扫过中间档位的那几帧 ——
             * 那几帧每个都撑不到 40 ms，自然不会被误锁。
             */
            in->press_at = in->run_at;
            in->long_sent = false;
            push(in, KEY_EV_PRESS, raw, mv, now_ms);
        }
        return;
    }

    /* ---- 情况二：读数仍是锁定的那个键 ---- */
    if (raw == in->locked) {
        if (!in->long_sent && now_ms - in->press_at >= KEY_LONG_PRESS_MS) {
            in->long_sent = true;
            push(in, KEY_EV_LONG, in->locked, mv, now_ms);
        }
        return;
    }

    /* ---- 情况三：读数跑出按键窗口（松开区，或读失败）----
     *
     * 要等满 120 ms 才算松手。按压过程中触点短暂断开、电压冲到松开区
     * 再回来是常事 —— 短暂失联既不该触发别的键，也不该打断正在按住的长按，
     * 所以这里**不动 press_at**，长按计时继续。 */
    if (raw == RAW_NONE) {
        if (stable >= KEY_LOCK_SWITCH_MS) {
            if (!in->long_sent) {
                push(in, KEY_EV_CLICK, in->locked, mv, now_ms);
            }
            push(in, KEY_EV_RELEASE, in->locked, mv, now_ms);
            in->locked = RAW_NONE;
            in->long_sent = false;
        }
        return;
    }

    /* ---- 情况四：读数落在另一个键的窗口里 ----
     *
     * 锁定期间一律忽略，除非它连续 120 ms —— 那才认作真的换键（而不是电气误读）。
     * 换键时先给老键补一条 RELEASE（**不补 CLICK**：它不是一次完整的短按），
     * 再锁新键。 */
    if (stable >= KEY_LOCK_SWITCH_MS) {
        push(in, KEY_EV_RELEASE, in->locked, mv, now_ms);
        in->locked = (int8_t)raw;
        /* 按下时刻不回溯到 run_at：这是异常路径，宁可让长按晚一点触发，
         * 也不要让它「一锁定就立刻长按」。 */
        in->press_at = now_ms;
        in->long_sent = false;
        push(in, KEY_EV_PRESS, raw, mv, now_ms);
    }
}

bool key_next(KeyInput *in, KeyMsg *out)
{
    if (!in || !out || in->q_n == 0) {
        return false;
    }
    *out = in->q[in->q_head];
    in->q_head = (uint8_t)((in->q_head + 1) % KEY_QUEUE);
    in->q_n--;
    return true;
}

bool key_is_down(const KeyInput *in)
{
    return in && in->locked != RAW_NONE;
}

KeyId key_held(const KeyInput *in)
{
    if (!in || in->locked == RAW_NONE) {
        return KEY_COUNT;
    }
    return (KeyId)in->locked;
}

const char *key_event_name(KeyEvent ev)
{
    switch (ev) {
    case KEY_EV_PRESS:   return "PRESS";
    case KEY_EV_CLICK:   return "CLICK";
    case KEY_EV_LONG:    return "LONG";
    case KEY_EV_RELEASE: return "RELEASE";
    default:             return "NONE";
    }
}

const char *key_name(KeyId key)
{
    switch (key) {
    case KEY_UP:   return "上";
    case KEY_DOWN: return "下";
    case KEY_OK:   return "确定";
    default:       return "-";
    }
}
