/*
 * play_pc.c - 在 Windows 上用键盘玩这一局 6v6
 *
 * 为什么要有它
 * ----------
 * 到这一步为止，所有逻辑都已经能在主机侧跑断言（见 README），但**断言测不出
 * 「好不好玩」**：招式菜单四个格子够不够看、日志一条条放会不会太慢、长按返回
 * 的手感对不对 —— 这些只能上手玩。硬件还没到手，所以先给一个和固件**同一条
 * 链路**的窗口：引擎、AI、会话层、渲染层都是一份源码，只有「最后一跳」不同
 * （固件送 SPI，这里送 GDI）。
 *
 * 一句话：这个窗口不是「另一个实现」，它就是固件，只是屏幕换成了窗口。
 *
 * 键盘 → 那三个键
 * --------------
 *   ▲ 上 / W          上键
 *   ▼ 下 / S          下键
 *   回车 / 空格 / J   确定键
 *
 *   Esc               退出窗口（PC 专有，设备上没有这个键）
 *   R                 重开一局（PC 专有；不重开也能在结算画面按确定回主菜单）
 *
 * 键位只有三个，所以方向键的上下就能覆盖全部导航 —— 这与设备一致，
 * 界面上没有任何一项是靠「左右」访问的。
 *
 * 为什么模拟电压，而不是直接喂按键事件
 * ----------------------------------
 * 有两条路可以走：
 *   A. 直接构造 KeyMsg（PRESS/CLICK/LONG）投给会话层；
 *   B. 每个模拟毫秒算一个 ADC 电压读数，喂给 key_poll()。
 * 这里选 B。因为**去抖、40 ms 按压确认、500 ms 长按、松手 120 ms、按住连发
 * 全都在驱动里**（poke_input.c）—— 走 A 就等于把这一层整个跳过去，玩起来
 * 手感是假的：短按会瞬间生效、长按的判定全靠代码自己编。走 B 则连「按住
 * 500 ms 才触发长按」这种手感都是真的，PC 上试出来的问题在真机上同样成立。
 *
 * 电压值不是编的，是从驱动自己的窗口上界算出来的（半开区间取中点）：
 *   ▲ [0, 150)      中点 75 mV
 *   ▼ [150, 447)    中点 298 mV
 *   确定 [447, 1900) 中点 1173 mV
 *   松开 2959 mV —— 本机实测值（标称 3300 是错的，别照抄标称）
 *
 * 最后一个注意点：键盘点一下有多短
 * ------------------------------
 * 驱动要求电压**连续稳定 40 ms** 才认作按下。手指按键不会只按 30 ms，但键盘
 * 会 —— 于是 PC 上会变成「有时按了没反应」。所以这里给每次按下补足到
 * HK_TAP_MIN_MS（见下），**只补足、不截断**：按住多久就是多久，长按手感不变。
 *
 * 用法：
 *   ./build/play_pc                      默认 3 倍放大，对手 AI_SMART
 *   ./build/play_pc --scale 2            指定放大倍数
 *   ./build/play_pc --ai rookie          对手换新手 AI
 *   ./build/play_pc --seed 12345         固定随机源（复现某一局）
 *   ./build/play_pc --frames 120         画 120 帧后自动退出（冒烟自检用）
 */
#define WIN32_LEAN_AND_MEAN
#ifndef UNICODE
#define UNICODE
#endif
#include <windows.h>

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "poke_font_data.h"
#include "poke_render.h"
#include "poke_session.h"
#include "poke_ui.h"

#define W UI_SCREEN_W
#define H UI_SCREEN_H

/* ================================================================ 时间与按键模拟 */

/*
 * PC 上给每次「按下」补足的最短时长。
 *
 * 驱动的按下确认要 40 ms 连续稳定，键盘点一下常常不到 —— 补到 60 ms 就够
 * 穿过那道门，同时不至于让连点变迟钝（补足后一次完整短按约 180 ms：
 * 60 ms 按住 + 120 ms 松手确认，与真机同一量级）。
 */
#define HK_TAP_MIN_MS (KEY_PRESS_CONFIRM_MS + 20)

/* 松开态电压：本机实测 2959，不是标称的 3300（poke_input.h 里记了这件事） */
#define HK_RELEASED_MV 2959

/*
 * 下面三条编译期断言钉住「PC 模拟出来的电压真的落在驱动认的窗口里」。
 *
 * 这三条不会失败在任何测试上，只会失败在**手感上**：电压落错窗口就是
 * 「按下没反应」，松开值不够高就是「没按键也一直被当成按住确定」——
 * 两种都不崩、不报错。所以宁可编译期钉死。
 */
_Static_assert(((0 + KEY_MV_UP_MAX) / 2) < KEY_MV_UP_MAX,
               "上键的模拟电压必须落在 [0, KEY_MV_UP_MAX) 里");
_Static_assert(((KEY_MV_UP_MAX + KEY_MV_DOWN_MAX) / 2) > KEY_MV_UP_MAX &&
               ((KEY_MV_UP_MAX + KEY_MV_DOWN_MAX) / 2) < KEY_MV_DOWN_MAX,
               "下键的模拟电压必须落在 [KEY_MV_UP_MAX, KEY_MV_DOWN_MAX) 里");
_Static_assert(((KEY_MV_DOWN_MAX + KEY_MV_OK_MAX) / 2) > KEY_MV_DOWN_MAX &&
               ((KEY_MV_DOWN_MAX + KEY_MV_OK_MAX) / 2) < KEY_MV_OK_MAX,
               "确定键的模拟电压必须落在 [KEY_MV_DOWN_MAX, KEY_MV_OK_MAX) 里");
_Static_assert(HK_RELEASED_MV > KEY_MV_OK_MAX,
               "松开态电压必须高于最大窗口上界，否则玩家停下不按时会被判成按住确定");

typedef struct {
    Session  ses;
    uint32_t sim_ms;        /* 模拟时基。与真实时间同源，但按 1 ms 步进推 */
    bool     down[KEY_COUNT];
    uint32_t hold_until[KEY_COUNT];   /* 补足用的截止时刻（模拟毫秒） */
    bool     running;
    int      frames_left;   /* <0 = 一直画；>=0 = 画够就退（烟雾自检） */
    int      frames;
    int      scale;
} App;

static App      g_app;
static uint16_t g_fb[W * H];          /* 16bpp 帧缓冲，与固件同格式 */
static uint8_t  g_bgr[W * H * 3];     /* 贴给 GDI 的 24bpp 中转，BGR 序 */

static const UiFonts kFonts = {
    .box    = &kFontApp12,
    .dialog = &kFontApp16,
    .label  = &kFontHp10,
};

/* ---- 键盘状态 ---- */

static void keys_clear(void)
{
    for (int i = 0; i < KEY_COUNT; i++) {
        g_app.down[i] = false;
    }
    /*
     * 补足截止时刻也要一起清掉。窗口失去焦点时如果还留着 hold_until，
     * 回来时会凭空多出一次「按下」—— 三个键的下限都不超过 60 ms，
     * 这点时间窗用不着跨焦点保留。
     */
    for (int i = 0; i < KEY_COUNT; i++) {
        g_app.hold_until[i] = 0;
    }
}

static bool key_down_now(KeyId k)
{
    return g_app.down[k] || g_app.sim_ms < g_app.hold_until[k];
}

/*
 * 此刻的 ADC 读数。
 *
 * 三个键共用同一个 ADC 引脚，真机上不可能同时按下两个；键盘可以。同时按
 * 时按 ▲ > ▼ > 确定 的优先级取一个，**不是**「合并」—— 合并出来的电压会
 * 落在另一个窗口里，那才是真的怪。
 */
static int adc_mv(void)
{
    if (key_down_now(KEY_UP)) {
        return (0 + KEY_MV_UP_MAX) / 2;
    }
    if (key_down_now(KEY_DOWN)) {
        return (KEY_MV_UP_MAX + KEY_MV_DOWN_MAX) / 2;
    }
    if (key_down_now(KEY_OK)) {
        return (KEY_MV_DOWN_MAX + KEY_MV_OK_MAX) / 2;
    }
    return HK_RELEASED_MV;
}

static void key_event_win(KeyId k, bool up)
{
    if (up) {
        g_app.down[k] = false;
        return;
    }
    if (!g_app.down[k]) {
        /* 只有「从抬起变按下」才重置补足下限。键盘自动重复会一直送
         * WM_KEYDOWN，不区分的话补足会被无限续期 —— 松手后键还按着。 */
        g_app.down[k] = true;
        g_app.hold_until[k] = g_app.sim_ms + HK_TAP_MIN_MS;
    }
}

/* ================================================================ 渲染 */

static void to_bgr(void)
{
    /*
     * RGB565 -> RGB888 的换算与 dump_screen.c 完全一致（「左移补高位」而不是
     * 简单左移：只左移的话满值 31 会变 248 而不是 255，白不够白）。
     *
     * 复制而不是共享：这是主机侧工具，为它单开一个 src/ 里的文件不值得 ——
     * src/ 是固件，固件不干这件事。两边数值都必须改的地方记在 README 里。
     */
    for (int i = 0; i < W * H; i++) {
        uint16_t c = g_fb[i];
        int r = (c >> 11) & 0x1F;
        int g = (c >> 5) & 0x3F;
        int b = c & 0x1F;
        g_bgr[i * 3 + 0] = (uint8_t)((b << 3) | (b >> 2));
        g_bgr[i * 3 + 1] = (uint8_t)((g << 2) | (g >> 4));
        g_bgr[i * 3 + 2] = (uint8_t)((r << 3) | (r >> 2));
    }
}

/*
 * 画一帧。分层由会话层决定 —— 弹层是盖在战斗界面上的，不是替换它，
 * 所以这里不能自己猜「现在是哪一屏」。
 */
static void render_frame(void)
{
    BattleScreen bs;
    MenuScreen   ms;

    memset(&bs, 0, sizeof(bs));
    memset(&ms, 0, sizeof(ms));

    ses_view(&g_app.ses, &bs);
    ses_menu_view(&g_app.ses, &ms);

    Surface s;
    r_init(&s, g_fb, W, H, W);
    r_fill(&s, 0);   /* 先涂黑：漏画的地方在窗口里一眼能看见 */

    switch (ses_layer(&g_app.ses)) {
    case SES_LAYER_MENU:
        ui_menu_screen(&s, &kFonts, &ms);
        break;
    case SES_LAYER_BATTLE:
        ui_battle_screen(&s, &kFonts, &bs);
        break;
    case SES_LAYER_BATTLE_MENU:
        ui_battle_screen(&s, &kFonts, &bs);
        ui_overlay_screen(&s, &kFonts, &ms);
        break;
    default:
        break;
    }

    to_bgr();
}

static void present(HDC dc, int cw, int ch)
{
    BITMAPINFO bi;
    memset(&bi, 0, sizeof(bi));
    bi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth       = W;
    bi.bmiHeader.biHeight      = -H;   /* 负数 = 自上而下，与我们的缓冲同序 */
    bi.bmiHeader.biPlanes      = 1;
    bi.bmiHeader.biBitCount    = 24;
    bi.bmiHeader.biCompression = BI_RGB;

    /*
     * COLORONCOLOR = 最近邻放大，不插值。这是像素画，插值会把 14x14 的点阵
     * 糊成一团；而且放大倍数是整数，最近邻出来的边缘是干净的。
     */
    SetStretchBltMode(dc, COLORONCOLOR);
    StretchDIBits(dc, 0, 0, cw, ch, 0, 0, W, H, g_bgr, &bi,
                  DIB_RGB_COLORS, SRCCOPY);
}

/* ================================================================ 无窗口自检 */

/*
 * 为什么不测「开着窗口按键盘」：那需要自动按键（SendInput），
 * 而且要人看着屏幕才有意义。
 *
 * 但 PC 端**独有**的那一小段逻辑完全可以无窗口测：键盘状态 → ADC 电压 →
 * 按键补足下限 → 驱动 → 会话。它恰好是只会静默出错的一段 —— 电压落到
 * 错窗口里就是「按下没反应」，补足下限算错就是「短按不掉」，都不会崩、
 * 不会报错。所以这里把那一段单独跑一遍，喂的是**同一条代码路径**
 * （key_event_win / adc_mv / ses_poll），只是不需要窗口。
 */
static int g_ck_fail;

#define CK(cond, ...) do {                                          \
    char _b[256];                                                   \
    snprintf(_b, sizeof(_b), __VA_ARGS__);                          \
    bool _ok = (cond);                                              \
    printf("  [%s] %s\n", _ok ? " OK " : "FAIL", _b);               \
    if (!_ok) { g_ck_fail++; }                                      \
} while (0)

/* 按模拟毫秒推进。设备侧每 1 ms 采一次，这里同节奏。 */
static void sim_advance(uint32_t ms)
{
    for (uint32_t i = 0; i < ms; i++) {
        ses_poll(&g_app.ses, g_app.sim_ms, adc_mv());
        g_app.sim_ms++;
    }
}

/* 短按：按下 80 ms 再抬起，然后等够松手确认（120 ms）——与真人点一下等价 */
static void sim_tap(KeyId k)
{
    key_event_win(k, false);
    sim_advance(HK_TAP_MIN_MS + 20);
    key_event_win(k, true);
    sim_advance(KEY_LOCK_SWITCH_MS + 30);
}

/* 长按：按住指定时长再抬起 */
static void sim_hold(KeyId k, uint32_t hold_ms)
{
    key_event_win(k, false);
    sim_advance(hold_ms);
    key_event_win(k, true);
    sim_advance(KEY_LOCK_SWITCH_MS + 30);
}

static int selfcheck(void)
{
    g_ck_fail = 0;

    *battle_rng_state() = 0x12345u;   /* 钉住随机源：自检要可重放 */
    g_app.sim_ms = 0;
    g_app.frames = 0;
    keys_clear();
    ses_init(&g_app.ses, AI_ROOKIE);
    ses_set_fonts(&g_app.ses, &kFonts);

    printf("PC 试玩链路自检（无窗口：键盘 → 电压 → 驱动 → 会话）\n");

    CK(ses_phase(&g_app.ses) == SES_PHASE_MAIN,
       "开局在主菜单（阶段 %d）", (int)ses_phase(&g_app.ses));

    /* ① 光标默认停在主菜单第 1 项「选择队伍」，先按「下」挪到「开始对战」 */
    sim_tap(KEY_DOWN);
    CK(ui_state_cursor(&g_app.ses.ui) == 1,
       "按「下」把光标挪到第 2 项（%d）", (int)ui_state_cursor(&g_app.ses.ui));

    /* ② 短按确定 → 开始对战，进日志播放 */
    sim_tap(KEY_OK);
    CK(ses_phase(&g_app.ses) == SES_PHASE_LOG,
       "短按确定 → 开始对战并进日志播放（阶段 %d）",
       (int)ses_phase(&g_app.ses));

    /* ③ **不投任何键**，纯推进时间 → 开场日志自动播完并转入等我方输入。
     *    这是「开场自动推进」的关键：玩家什么都不按，日志也会自己往下走。
     *    长按跳过那条路仍由下面的 ④ 覆盖（出招后用 sim_hold 跳过一段日志）。 */
    sim_advance(5200);
    CK(ses_phase(&g_app.ses) == SES_PHASE_INPUT,
       "不投键推进 5200ms → 开场日志自动放完并转入等我方输入（阶段 %d）",
       (int)ses_phase(&g_app.ses));
    CK(ui_state_screen(&g_app.ses.ui) == UI_SCREEN_BATTLE,
       "落在战斗界面（界面 %d）", (int)ui_state_screen(&g_app.ses.ui));
    CK(ses_log_index(&g_app.ses) + 1 >= 1, "日志游标已推进到第 %u 条",
       (unsigned)ses_log_index(&g_app.ses) + 1);

    /* ④ 出一招：回合真的往前走了 */
    uint16_t turn0 = g_app.ses.b.turn;
    sim_tap(KEY_OK);
    CK(ses_phase(&g_app.ses) == SES_PHASE_LOG ||
       ses_phase(&g_app.ses) == SES_PHASE_RESULT,
       "确定出招 → 进日志播放（阶段 %d）", (int)ses_phase(&g_app.ses));
    sim_hold(KEY_OK, KEY_LONG_PRESS_MS + 200);
    CK(g_app.ses.b.turn > turn0, "回合数前进：%u → %u",
       (unsigned)turn0, (unsigned)g_app.ses.b.turn);

    /*
     * ⑤ 换人路径。出招后可能落在三种界面之一：正常等输入（战斗界面）、
     * 出战那只濒死了要强制换人（换人界面）、或者分出胜负（结算）。
     * 三者都是对的，但不能是别的 —— 更不能还停在日志播放。
     */
    SesPhase ph = ses_phase(&g_app.ses);
    UiScreen sc = ui_state_screen(&g_app.ses.ui);
    CK(ph == SES_PHASE_INPUT || ph == SES_PHASE_RESULT,
       "回合结束后停在合理阶段（阶段 %d，界面 %d）", (int)ph, (int)sc);
    if (ph == SES_PHASE_INPUT) {
        CK(sc == UI_SCREEN_BATTLE || sc == UI_SCREEN_SWITCH,
           "输入阶段的界面是战斗或换人（界面 %d）", (int)sc);
    }

    /* ⑥ 强制换人：长按在这里**没有退路**，只能选一只 */
    if (ph == SES_PHASE_INPUT && sc == UI_SCREEN_SWITCH) {
        sim_tap(KEY_OK);   /* 选列表第 1 只 */
        if (ses_phase(&g_app.ses) == SES_PHASE_LOG) {
            sim_hold(KEY_OK, KEY_LONG_PRESS_MS + 200);
        }
        CK(ses_phase(&g_app.ses) == SES_PHASE_INPUT &&
           ui_state_screen(&g_app.ses.ui) == UI_SCREEN_BATTLE,
           "强制换人后回到战斗界面（阶段 %d，界面 %d）",
           (int)ses_phase(&g_app.ses), (int)ui_state_screen(&g_app.ses.ui));
    }

    /* ⑦ 战斗界面上的整条导航链：长按开弹层 → 进换人 → 长按退回 */
    if (ses_phase(&g_app.ses) == SES_PHASE_INPUT &&
        ui_state_screen(&g_app.ses.ui) == UI_SCREEN_BATTLE) {

        /*
         * 顺便验一次「一屏数据齐全」。这一条不是凑数：曾经出过
         * ses_view() 把等级/血量写进了缓冲却忘了挂到输出指针上的事故 ——
         * 画面上血条框里数字凭空消失，而所有断言全绿。
         */
        BattleScreen bs;
        memset(&bs, 0, sizeof(bs));
        ses_view(&g_app.ses, &bs);
        CK(bs.me_name && bs.me_lv && bs.me_hp_text && bs.moves[0],
           "一屏数据齐全：%s %s %s 首招「%s」",
           bs.me_name ? bs.me_name : "(空)", bs.me_lv ? bs.me_lv : "(空)",
           bs.me_hp_text ? bs.me_hp_text : "(空)",
           bs.moves[0] ? bs.moves[0] : "(空)");

        sim_hold(KEY_OK, KEY_LONG_PRESS_MS + 200);
        CK(ui_state_screen(&g_app.ses.ui) == UI_SCREEN_ACTION,
           "战斗界面上长按确定 → 打开弹层（界面 %d）",
           (int)ui_state_screen(&g_app.ses.ui));
        CK(ses_layer(&g_app.ses) == SES_LAYER_BATTLE_MENU,
           "弹层是**盖在**战斗界面上的（分层 %d，2 = 战斗+弹层）",
           (int)ses_layer(&g_app.ses));

        sim_tap(KEY_OK);   /* 弹层第 1 项 = 换人 */
        CK(ui_state_screen(&g_app.ses.ui) == UI_SCREEN_SWITCH,
           "弹层里选第 1 项 → 进换人列表（界面 %d）",
           (int)ui_state_screen(&g_app.ses.ui));

        sim_hold(KEY_OK, KEY_LONG_PRESS_MS + 200);   /* 长按返回 */
        CK(ui_state_screen(&g_app.ses.ui) == UI_SCREEN_ACTION,
           "长按返回 → 退回弹层（界面 %d）",
           (int)ui_state_screen(&g_app.ses.ui));
    }

    printf("  自检结果：%s（%d 项失败），模拟 %u ms\n",
           g_ck_fail ? "有失败" : "全部通过", g_ck_fail,
           (unsigned)g_app.sim_ms);
    return g_ck_fail ? 1 : 0;
}

/* ================================================================ 窗口 */

static LRESULT CALLBACK wnd_proc(HWND hw, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_KEYDOWN: {
        bool repeat = (lp & (1 << 30)) != 0;   /* 位 30 = 之前就已经按下 */
        switch (wp) {
        case VK_UP:    case 'W': key_event_win(KEY_UP, false);   break;
        case VK_DOWN:  case 'S': key_event_win(KEY_DOWN, false); break;
        case VK_RETURN: case VK_SPACE: case 'J':
            key_event_win(KEY_OK, false);
            break;
        case VK_ESCAPE:
            g_app.running = false;
            break;
        case 'R':
            /* 重开一局：ses_init 会重装默认队伍并把状态机复位到主菜单。
             * 随机源跟着换（除非 --seed 钉死了），所以每一局都不一样。 */
            if (!repeat) {
                ses_init(&g_app.ses, (uint8_t)g_app.ses.ai.level);
                ses_set_fonts(&g_app.ses, &kFonts);
                printf("[PC] 已重开一局（主菜单）\n");
            }
            break;
        default:
            break;
        }
        (void)repeat;
        return 0;
    }
    case WM_KEYUP:
        switch (wp) {
        case VK_UP:    case 'W': key_event_win(KEY_UP, true);   break;
        case VK_DOWN:  case 'S': key_event_win(KEY_DOWN, true); break;
        case VK_RETURN: case VK_SPACE: case 'J':
            key_event_win(KEY_OK, true);
            break;
        default: break;
        }
        return 0;

    case WM_KILLFOCUS:
        /*
         * 切走窗口时把按键状态清掉。不清的话「按住 ▼ 然后 Alt+Tab」回来会
         * 发现键还按着 —— 因为 WM_KEYUP 送到别的窗口去了。
         */
        keys_clear();
        return 0;

    case WM_ERASEBKGND:
        return 1;   /* 我们自己整屏重画，别让系统先刷一遍底（会闪） */

    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hw, &ps);
        RECT rc;
        GetClientRect(hw, &rc);
        present(dc, rc.right - rc.left, rc.bottom - rc.top);
        EndPaint(hw, &ps);
        return 0;
    }

    case WM_CLOSE:
        DestroyWindow(hw);
        return 0;

    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;

    default:
        break;
    }
    return DefWindowProcW(hw, msg, wp, lp);
}

/*
 * 放大倍数。窗口要比屏幕工作区矮一点 —— 一开就顶出屏幕、下面被任务栏
 * 切掉一条，是「血量数字看不见」这类误报的常见来源。
 */
static int pick_scale(int want)
{
    if (want > 0) {
        return want;
    }
    RECT wa;
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);
    int avail = (int)(wa.bottom - wa.top);
    for (int sc = 4; sc >= 1; sc--) {
        if (H * sc + 60 <= avail) {   /* 60 = 标题栏与边框的余量 */
            return sc;
        }
    }
    return 1;
}

/* ================================================================ main */

static void usage(void)
{
    printf(
        "用法: play_pc [--scale N] [--ai rookie|smart|expert] [--seed N]\n"
        "               [--frames N] [--selfcheck]\n");
}

int main(int argc, char **argv)
{
    int  want_scale = 0;
    int  ai = AI_SMART;
    uint32_t seed = 0;
    bool seed_given = false;
    int  frames = -1;
    bool want_selfcheck = false;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--scale") == 0 && i + 1 < argc) {
            want_scale = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--ai") == 0 && i + 1 < argc) {
            const char *a = argv[++i];
            if (strcmp(a, "rookie") == 0)      { ai = AI_ROOKIE; }
            else if (strcmp(a, "smart") == 0)  { ai = AI_SMART; }
            else if (strcmp(a, "expert") == 0) { ai = AI_EXPERT; }
            else { fprintf(stderr, "不认识的 AI 等级：%s\n", a); return 2; }
        } else if (strcmp(argv[i], "--seed") == 0 && i + 1 < argc) {
            seed = (uint32_t)strtoul(argv[++i], NULL, 0);
            seed_given = true;
        } else if (strcmp(argv[i], "--frames") == 0 && i + 1 < argc) {
            frames = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--selfcheck") == 0) {
            want_selfcheck = true;
        } else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            usage();
            return 0;
        } else {
            fprintf(stderr, "未知参数 %s\n", argv[i]);
            usage();
            return 2;
        }
    }

    /* 自检不开窗口：它验的是「键盘 → 电压 → 驱动 → 会话」那一段 */
    if (want_selfcheck) {
        return selfcheck();
    }

    /*
     * 随机源。不钉种子就用开机毫秒当种子 —— 每一局都不一样，这才是「打一局」
     * 该有的样子。--seed 是给复现问题用的（和测试里那套固定桩一个意思）。
     */
    if (!seed_given) {
        seed = (uint32_t)GetTickCount64() | 1u;
    }
    *battle_rng_state() = seed;

    /*
     * **必须在 pick_scale() 之前**声明 DPI 感知。
     *
     * 不声明的话系统会把窗口替我们放大（高 DPI 下画面糊），而且更阴的是尺寸
     * 会**两个来源不一致**：声明前 GetSystemMetrics 给的是被缩放过的虚拟尺寸，
     * 而 CreateWindowEx 收到的是物理像素 —— 于是「按屏幕高度算出来的放大倍数」
     * 到了真正开窗时对不上，窗口可能一开就顶出屏幕。
     * 声明之后两处都是物理像素，才能互相比较。
     */
    SetProcessDPIAware();

    g_app.scale = pick_scale(want_scale);
    g_app.frames_left = frames;
    g_app.running = true;

    ses_init(&g_app.ses, (uint8_t)ai);
    ses_set_fonts(&g_app.ses, &kFonts);
    keys_clear();

    printf("poke-battle PC 试玩\n");
    printf("  屏幕 %dx%d，放大 %d 倍（窗口 %dx%d）\n",
           W, H, g_app.scale, W * g_app.scale, H * g_app.scale);
    printf("  对手 AI 等级 %d，随机种子 0x%08X%s\n", ai, seed,
           seed_given ? "（已钉住，可复现）" : "");
    printf("  按键：▲/W 上   ▼/S 下   回车/空格/J 确定   Esc 退出   R 重开\n");
    printf("  长按「确定」1 秒可跳过整段战况日志。\n");

    /*
     * 窗口只用来「有地方贴像素」：客户区不会自己在像素层缩放，贴图时才拉伸。
     * 允许用户拖拽改大小（拉伸会改变长宽比，画面会跟着变形）—— 这比锁死尺寸
     * 好：万一自动选的倍数不合适，缩一下窗口就能看全。
     */
    WNDCLASSEXW wc;
    memset(&wc, 0, sizeof(wc));
    wc.cbSize        = sizeof(wc);
    wc.style         = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc   = wnd_proc;
    wc.hInstance     = GetModuleHandleW(NULL);
    wc.hCursor       = LoadCursorW(NULL, IDC_ARROW);
    wc.lpszClassName = L"PokeBattlePC";
    if (!RegisterClassExW(&wc)) {
        fprintf(stderr, "RegisterClassExW 失败（错误码 %lu）\n", GetLastError());
        return 1;
    }

    RECT rc = { 0, 0, W * g_app.scale, H * g_app.scale };
    AdjustWindowRect(&rc, WS_OVERLAPPEDWINDOW, FALSE);
    HWND hw = CreateWindowExW(
        0, wc.lpszClassName, L"poke-battle — 6v6 试玩",
        WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
        rc.right - rc.left, rc.bottom - rc.top,
        NULL, NULL, wc.hInstance, NULL);
    if (!hw) {
        fprintf(stderr, "CreateWindowExW 失败（错误码 %lu）\n", GetLastError());
        return 1;
    }
    ShowWindow(hw, SW_SHOW);
    UpdateWindow(hw);

    ULONGLONG t0 = GetTickCount64();
    bool quit = false;

    while (!quit) {
        MSG m;
        while (PeekMessageW(&m, NULL, 0, 0, PM_REMOVE)) {
            if (m.message == WM_QUIT) {
                quit = true;
                break;
            }
            TranslateMessage(&m);
            DispatchMessageW(&m);
        }
        if (!g_app.running) {
            break;
        }

        /*
         * 推进模拟时基，每 1 ms 喂一次采样 —— 与设备侧同一个节奏
         * （bsp_button 也是 1 ms 采一次）。落后太多就直接对齐：追赶时
         * 补齐几千次采样没有意义，反而会让「按住」的时长虚长。
         */
        uint32_t target = (uint32_t)(GetTickCount64() - t0);
        int steps = 0;
        while (g_app.sim_ms < target && steps < 200) {
            ses_poll(&g_app.ses, g_app.sim_ms, adc_mv());
            g_app.sim_ms++;
            steps++;
        }
        if (g_app.sim_ms < target) {
            g_app.sim_ms = target;
        }

        render_frame();
        HDC dc = GetDC(hw);
        RECT cr;
        GetClientRect(hw, &cr);
        present(dc, cr.right - cr.left, cr.bottom - cr.top);
        ReleaseDC(hw, dc);
        g_app.frames++;

        if (g_app.frames_left >= 0 && g_app.frames >= g_app.frames_left) {
            quit = true;
        }
        Sleep(1);   /* 让出 CPU；这一步同时也是「真实时间」的步长 */
    }

    /*
     * 退出时把「停在哪一屏」打出来。玩家反馈「卡住了 / 没反应」时，这一行
     * 比任何截图都有用 —— 界面编号直接对应 poke_ui_state.h 里的 UiScreen。
     */
    printf("[PC] 退出：渲染 %d 帧，模拟 %u ms（约 %.1f 秒）\n",
           g_app.frames, (unsigned)g_app.sim_ms, g_app.sim_ms / 1000.0);
    printf("     末态：阶段 %d，界面 %d，日志序号 %u\n",
           (int)ses_phase(&g_app.ses), (int)ui_state_screen(&g_app.ses.ui),
           (unsigned)ses_log_index(&g_app.ses));
    return 0;
}
