/*
 * test_input.c - 三键驱动自检
 *
 * 为什么这一层必须能在主机侧测
 * ---------------------------
 * 去抖、长按、电气误读这三类问题的共同点是：**在真机上只能靠手感判断**。
 * 「按住上键偶尔弹出菜单」这种症状，不看 mV 根本说不清是应用 bug 还是电气问题。
 * 官方文档里那台设备就是靠事后回看「事件 + 当时的 mV」才定位的。
 *
 * 所以这里用「电压序列 + 毫秒时基」当信号发生器，把下面这些钉死：
 *   * 窗口边界（150 / 447 / 1900 三处）
 *   * 按下要连续稳定 40 ms 才认（挡掉按下瞬间扫过中间档位的几帧）
 *   * 长按 500 ms、松手确认 120 ms
 *   * 按住时触点短暂断开（电压冲回松开区）不能打断长按
 *   * 落在别的窗口要连续 120 ms 才算换键
 *   * **ADC 读失败必须当「未按下」**（当成 0 mV 就是「上键一直被按住」）
 */
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "poke_input.h"

static int g_pass = 0, g_fail = 0;

static void check(int cond, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    char buf[240];
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (cond) {
        g_pass++;
        printf("  [ OK ] %s\n", buf);
    } else {
        g_fail++;
        printf("  [FAIL] %s\n", buf);
    }
}

/* ================================================================ 信号发生器 */

/* 用真实电压：档位取自 bsp_pins.h 的分压计算；松开取**标称 3300** 与
 * **本机实测 2959** 两个值，因为「照抄标称值」正是官方文档点名的坑。 */
#define MV_UP        0
#define MV_DOWN      300
#define MV_OK        595
#define MV_OPEN      3300
#define MV_OPEN_REAL 2959

typedef struct {
    KeyInput in;
    uint32_t t;
    bool     drain;     /* 是否每毫秒排空队列（测队列溢出时关掉） */
    KeyMsg   evs[128];
    int      n;
} Rig;

static void rig_reset(Rig *r)
{
    key_init(&r->in);
    r->t = 0;
    r->drain = true;
    r->n = 0;
}

static void drain_all(Rig *r)
{
    KeyMsg m;
    while (key_next(&r->in, &m)) {
        if (r->n < (int)(sizeof(r->evs) / sizeof(r->evs[0]))) {
            r->evs[r->n++] = m;
        }
    }
}

/* 保持某个电压 ms 毫秒。每毫秒一次采样 —— 与官方 BSP 的采样周期一致。 */
static void hold(Rig *r, int mv, int ms)
{
    for (int i = 0; i < ms; i++) {
        r->t++;
        key_poll(&r->in, r->t, mv);
        if (r->drain) {
            drain_all(r);
        }
    }
}

static void clear_ev(Rig *r)
{
    r->n = 0;
}

/* 事件名。用 4 个轮转缓冲，避免一条断言里多次调用互相覆盖。 */
static const char *ev_str(const KeyMsg *m)
{
    static char buf[4][64];
    static int idx;
    idx = (idx + 1) % 4;
    snprintf(buf[idx], sizeof(buf[idx]), "%s@%ums(%dmV)",
             m->ev == KEY_EV_PRESS ? "PRESS" :
             m->ev == KEY_EV_CLICK ? "CLICK" :
             m->ev == KEY_EV_LONG  ? "LONG"  :
             m->ev == KEY_EV_RELEASE ? "RELEASE" : "?",
             (unsigned)m->t_ms, m->mv);
    return buf[idx];
}

/* 只关心「按住某个电压 50 ms 后识别成了哪个键」 */
static KeyId press_key(int mv)
{
    Rig r;
    rig_reset(&r);
    hold(&r, mv, 50);
    return (r.n >= 1 && r.evs[0].ev == KEY_EV_PRESS) ? r.evs[0].key : KEY_COUNT;
}

/* ================================================================ 用例 */

static void test_windows(void)
{
    printf("\n[窗口边界] 半开区间，边界取相邻档中点\n");

    check(press_key(MV_UP) == KEY_UP, "0 mV → 上（窗口 [0,150)）");
    check(press_key(149) == KEY_UP, "149 mV → 上（上界是开区间）");
    check(press_key(150) == KEY_DOWN, "150 mV → 下（边界归下一档）");
    check(press_key(300) == KEY_DOWN, "300 mV → 下（标称值）");
    check(press_key(446) == KEY_DOWN, "446 mV → 下");
    check(press_key(447) == KEY_OK, "447 mV → 确定");
    check(press_key(595) == KEY_OK, "595 mV → 确定（标称值）");
    check(press_key(1899) == KEY_OK, "1899 mV → 确定（上界是开区间）");

    check(press_key(MV_OPEN) == KEY_COUNT, "3300 mV（标称松开）→ 无键");
    check(press_key(MV_OPEN_REAL) == KEY_COUNT, "2959 mV（本机实测松开）→ 无键");
    check(press_key(1900) == KEY_COUNT, "1900 mV → 无键（松开阈值就是它）");

    check(press_key(-1) == KEY_COUNT,
          "ADC 读失败（-1）→ 无键（当成 0 mV 就变成「上键被按住」了）");
    check(press_key(-42) == KEY_COUNT, "任意负值都当读失败 → 无键");
}

static void test_confirm(void)
{
    printf("\n[按下确认] 窗口要连续稳定 40 ms\n");

    Rig r;
    rig_reset(&r);
    hold(&r, MV_UP, 39);
    check(r.n == 0, "按住 39 ms 还不报事件（当前 %d 条）", r.n);

    hold(&r, MV_UP, 2);
    check(r.n == 1 && r.evs[0].ev == KEY_EV_PRESS,
          "第 41 ms 报 PRESS（实际 %d 条）", r.n);
    check(r.n == 1 && r.evs[0].t_ms == 41,
          "PRESS 时刻 = 电压进入窗口那刻 + 40 ms（实际 %ums）",
          r.n ? r.evs[0].t_ms : 0);
    check(r.n == 1 && r.evs[0].mv == MV_UP,
          "事件带着判定用的那次采样值（%d mV）", r.n ? r.evs[0].mv : -999);

    /* 稳定期内换档要重新起算，不能把两段时长加起来 */
    rig_reset(&r);
    hold(&r, MV_UP, 30);
    hold(&r, MV_DOWN, 30);   /* 换档 */
    check(r.n == 0, "两段各 30 ms 但中途换档 → 仍不报（%d 条）", r.n);
    hold(&r, MV_DOWN, 12);
    check(r.n == 1 && r.evs[0].key == KEY_DOWN,
          "下键窗口重新起算后满 40 ms → PRESS(下)（%s）",
          r.n ? ev_str(&r.evs[0]) : "-");
}

static void test_sweep(void)
{
    printf("\n[按下瞬间] 电压扫过中间档位不应被误读\n");

    /* 三个键共用一个引脚，按下那一刻电压从松开态冲向目标档，途中会依次
     * 穿过确定、下的窗口。每档只停几毫秒，都撑不到 40 ms，所以都不该被锁上。 */
    Rig r;
    rig_reset(&r);
    hold(&r, MV_OPEN, 50);
    clear_ev(&r);
    hold(&r, MV_OK, 5);      /* 扫过确定档 5 ms */
    hold(&r, MV_DOWN, 6);    /* 扫过下档 6 ms */
    hold(&r, MV_UP, 60);     /* 真正按下上键 */
    check(r.n == 1 && r.evs[0].ev == KEY_EV_PRESS && r.evs[0].key == KEY_UP,
          "扫过两档后只报一次 PRESS(上)（%d 条）", r.n);
    check(r.n == 1 && r.evs[0].t_ms == 102,
          "PRESS 时刻按「进入上键窗口」起算：上键窗口自第 62 ms 起，+40 = 102（实际 %ums）",
          r.n ? r.evs[0].t_ms : 0);
}

static void test_click_long(void)
{
    printf("\n[短按/长按]\n");

    Rig r;
    rig_reset(&r);
    hold(&r, MV_UP, 100);
    check(r.n == 1 && r.evs[0].ev == KEY_EV_PRESS, "先只有 PRESS（%d 条）", r.n);
    hold(&r, MV_UP, 200);   /* 累计 300 ms，仍不到长按 */
    check(r.n == 1, "按住 300 ms 还没到长按（%d 条）", r.n);
    hold(&r, MV_OPEN, 130); /* 松开：等 120 ms 确认 */
    check(r.n == 3 && r.evs[1].ev == KEY_EV_CLICK && r.evs[2].ev == KEY_EV_RELEASE,
          "松开后 CLICK + RELEASE（共 %d 条）", r.n);
    check(r.n == 3 && r.evs[1].t_ms == 421,
          "CLICK 在「进入松开区 = 第 301 ms」+ 120 ms 时产生（实际 %ums）",
          r.n > 1 ? r.evs[1].t_ms : 0);
    check(r.n == 3 && r.evs[1].key == KEY_UP && r.evs[2].key == KEY_UP,
          "CLICK 与 RELEASE 都带键位（%s / %s）",
          r.n > 2 ? ev_str(&r.evs[1]) : "-", r.n > 2 ? ev_str(&r.evs[2]) : "-");
    check(!key_is_down(&r.in), "松开后 key_is_down() 为假");
    check(key_held(&r.in) == KEY_COUNT, "松开后 key_held() 无键");

    /* 轻点也要算一次 CLICK：180 ms 那档是组件区分单击/长按的边界，
     * 本层真正的长按边界是 500 ms。三键设备上「确定」必须一按就响应，
     * 不能再叠一道最小按压时长。 */
    rig_reset(&r);
    hold(&r, MV_OK, 60);
    hold(&r, MV_OPEN, 130);
    check(r.n == 3 && r.evs[1].ev == KEY_EV_CLICK,
          "66 ms 的轻点也算一次 CLICK（%d 条）", r.n);

    /* 长按 */
    rig_reset(&r);
    hold(&r, MV_OK, 700);
    uint32_t press_at = r.n >= 1 ? r.evs[0].t_ms - KEY_PRESS_CONFIRM_MS : 0;
    check(r.n == 2 && r.evs[0].ev == KEY_EV_PRESS && r.evs[1].ev == KEY_EV_LONG,
          "按住 700 ms → PRESS + LONG（共 %d 条）", r.n);
    check(r.n == 2 && r.evs[1].t_ms == press_at + KEY_LONG_PRESS_MS,
          "LONG 在「按下时刻 + 500 ms」产生（实际 %ums，应为 %u）",
          r.n > 1 ? r.evs[1].t_ms : 0, (unsigned)(press_at + KEY_LONG_PRESS_MS));
    clear_ev(&r);
    hold(&r, MV_OK, 300);   /* 长按之后继续按住 */
    check(r.n == 0, "LONG 只报一次，继续按住不再产生事件（%d 条）", r.n);

    /* 长按之后抬起：只补 RELEASE，**不补 CLICK**
     * —— 否则「长按返回」会顺手把光标那项也确定一次。 */
    hold(&r, MV_OPEN, 130);
    check(r.n == 1 && r.evs[0].ev == KEY_EV_RELEASE,
          "长按后抬起只补 RELEASE、不补 CLICK（%d 条）", r.n);
}

static void test_glitch(void)
{
    printf("\n[电气抖动] 按住时触点短暂断开\n");

    /* 症状是「短按一切正常，只有长按出问题」：按住时触点断开几毫秒，
     * 电压朝松开态跑，途中穿过上方每一个窗口，于是被读成「另一个键按了一下」。
     * 短于 120 ms 的一律忽略。 */
    Rig r;
    rig_reset(&r);
    hold(&r, MV_UP, 200);
    uint32_t press_at = r.evs[0].t_ms - KEY_PRESS_CONFIRM_MS;
    clear_ev(&r);
    hold(&r, MV_OPEN, 60);   /* 断开 60 ms（< 120，不算松手） */
    hold(&r, MV_OK, 40);     /* 途中穿过确定档（< 120，不算换键） */
    hold(&r, MV_OPEN, 50);   /* 又断 */
    hold(&r, MV_UP, 400);    /* 回到按住 */
    check(r.n == 1 && r.evs[0].ev == KEY_EV_LONG,
          "抖动期间只报 LONG：没有跨键事件，也没有 CLICK（%d 条）", r.n);
    check(r.n == 1 && r.evs[0].t_ms == press_at + KEY_LONG_PRESS_MS,
          "抖动不重置长按计时（LONG 仍在按下 + 500 ms，实际 %ums）",
          r.n ? r.evs[0].t_ms : 0);

    /* 反面：真的换到另一个窗口并稳定超过 120 ms —— 这时才认换键。
     * 这是键位锁的边界，也是它的局限：更长时间的电气断开确实会被当成换键。
     * 所以「按住持续生效」的操作要用应用自己的 tick，不能靠驱动的电平。 */
    rig_reset(&r);
    hold(&r, MV_UP, 200);
    clear_ev(&r);
    hold(&r, MV_OK, 130);
    check(r.n == 2 && r.evs[0].ev == KEY_EV_RELEASE && r.evs[0].key == KEY_UP
          && r.evs[1].ev == KEY_EV_PRESS && r.evs[1].key == KEY_OK,
          "另一窗口稳定满 120 ms 才认换键：RELEASE(上) + PRESS(确定)（%d 条）", r.n);
    check(r.n == 2, "换键不给老键补 CLICK（它是异常中断，不是一次完整短按）");
}

static void test_queue(void)
{
    printf("\n[事件队列] 满了丢事件并计数，绝不阻塞\n");

    Rig r;
    rig_reset(&r);
    r.drain = false;   /* 不排空：逼出队列溢出 */
    for (int i = 0; i < 3; i++) {
        hold(&r, MV_OK, 60);     /* PRESS */
        hold(&r, MV_OPEN, 130);  /* CLICK + RELEASE */
    }
    r.drain = true;
    drain_all(&r);

    check(r.in.dropped == 1, "队列深 8、产 9 条 → 丢 1 条（实际丢 %u 条）",
          (unsigned)r.in.dropped);
    check(r.n == 8, "取出的正好是队列深度的 8 条（实际 %d 条）", r.n);
    check(r.evs[0].ev == KEY_EV_PRESS && r.evs[1].ev == KEY_EV_CLICK
          && r.evs[2].ev == KEY_EV_RELEASE,
          "先来的顺序不乱，丢的是最末那条（%s → %s → %s）",
          ev_str(&r.evs[0]), ev_str(&r.evs[1]), ev_str(&r.evs[2]));
}

static void test_misc(void)
{
    printf("\n[杂项] 防御性与查询\n");

    Rig r;
    rig_reset(&r);
    check(!key_is_down(&r.in), "复位后未按下");
    check(key_held(&r.in) == KEY_COUNT, "复位后 key_held() 无键");
    check(r.in.samples == 0, "复位后采样计数为 0");
    hold(&r, MV_OPEN, 10);
    check(r.in.samples == 10, "采样计数随采样递增（%u）", (unsigned)r.in.samples);

    hold(&r, MV_UP, 60);
    check(key_is_down(&r.in) && key_held(&r.in) == KEY_UP, "按住上键时查询得到键位");

    /* 空指针一律不崩 */
    key_init(NULL);
    key_poll(NULL, 1, 0);
    check(!key_next(NULL, &r.evs[0]), "key_next(NULL) 返回假");
    check(!key_is_down(NULL) && key_held(NULL) == KEY_COUNT,
          "空指针查询不崩且返回「无键」");
    KeyMsg m;
    check(!key_next(&r.in, NULL), "key_next(队列, NULL) 返回假");

    check(strcmp(key_event_name(KEY_EV_LONG), "LONG") == 0,
          "事件名可读：%s", key_event_name(KEY_EV_LONG));
    check(strcmp(key_name(KEY_UP), "上") == 0, "键名可读：%s", key_name(KEY_UP));

    rig_reset(&r);
    check(!key_next(&r.in, &m), "空队列取不到事件");
}

int main(void)
{
    printf("三键驱动自检\n");
    printf("============\n");

    test_windows();
    test_confirm();
    test_sweep();
    test_click_long();
    test_glitch();
    test_queue();
    test_misc();

    printf("\n=== 结果: %d 通过, %d 失败 ===\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
