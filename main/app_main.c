/*
 * 固件入口 —— 把 PC 上已经跑通的三层接到真机。
 *
 * 与 test/play_pc.c 的关系
 * -----------------------
 * 同一份链路：引擎、AI、会话层、绘制层都是 src/ 的源码，区别只在最后一跳：
 *   PC      RGB565 整屏缓冲 → GDI 贴图
 *   真机    240x64 条带缓冲 → esp_lcd_panel_draw_bitmap(bsp_display_panel())
 * 条带是为了省 RAM：整数屏 150KB，C3 只有 400KB SRAM。绘制层本来就支持
 * 「缓冲带 origin + 裁剪」（见 poke_render.h），所以每屏画 5 条即可。
 *
 * 输入
 * ----
 * 不用官方 BSP 的按键回调：本项目的 key_poll() 只吃「一个 mV 读数 + 一个
 * 毫秒时基」，每 1ms 调一次 bsp_button_read_mv() 即可。这样去抖 / 40ms
 * 按压确认 / 500ms 长按 / 键位锁在真机与 PC 上是同一套代码。
 *
 * 刷新策略
 * --------
 * 会话层的状态变化用一份「指纹」判断：阶段、界面、光标、日志序号、等待
 * 动效相位、回合数。变了才整屏重画 —— 静止画面不重复占 SPI 带宽。
 */
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "esp_attr.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"

#include "bsp_button.h"
#include "bsp_display.h"

#include "poke_font_data.h"
#include "poke_render.h"
#include "poke_session.h"
#include "poke_ui.h"

static const char *TAG = "poke";

/* 条带高度取 64：240*64*2 = 30KB，5 条覆盖 320 行。 */
#define STRIPE_H 64
#define STRIPES   (UI_SCREEN_H / STRIPE_H + ((UI_SCREEN_H % STRIPE_H) ? 1 : 0))

/* 每 16ms 一次按住连发（与 PC 侧同节奏）。 */
#define TICK_MS 16

/* 主循环落后超过这么多毫秒就直接对齐 —— 补齐几千次 ADC 读没有意义，
 * 反而会让「按住」的时长虚长（同 play_pc.c 的做法）。 */
#define CATCHUP_LIMIT_MS 100

static uint16_t s_stripe[UI_SCREEN_W * STRIPE_H];
static Session  s_ses;

static SemaphoreHandle_t s_flush_done;

static const UiFonts kFonts = {
    .box    = &kFontApp12,
    .dialog = &kFontApp16,
    .label  = &kFontHp10,
};

/* ---------------------------------------------------------------- 时基 */

static uint32_t now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

/* ---------------------------------------------------------------- 刷屏 */

/* esp_lcd 的 color 传输是异步的（DMA + 中断），缓冲在回调前不能改写。
 * 条带缓冲只有一块，所以每发一条都要等到传输完成再接着画下一条。 */
static bool IRAM_ATTR on_color_trans_done(esp_lcd_panel_io_handle_t io,
                                          esp_lcd_panel_io_event_data_t *edata,
                                          void *user_ctx)
{
    (void)io;
    (void)edata;
    (void)user_ctx;
    BaseType_t hp = pdFALSE;
    xSemaphoreGiveFromISR(s_flush_done, &hp);
    return hp == pdTRUE;
}

/*
 * RGB565 字节序。
 * 绘制层按小端 uint16 存；ST7789 走 SPI 要求高字节在前 —— 与官方 LVGL 层
 * 的 swap_bytes = true 是同一件事（见 ai-passport 的 bsp_display_lvgl.c）。
 */
static void swap_bytes(uint16_t *p, int n)
{
    for (int i = 0; i < n; i++) {
        uint16_t v = p[i];
        p[i] = (uint16_t)((v >> 8) | (v << 8));
    }
}

static void flush_stripe(int y, int h)
{
    swap_bytes(s_stripe, UI_SCREEN_W * h);

    /* 清掉可能残留的上一块完成信号，避免把旧信号当成本块的。 */
    (void)xSemaphoreTake(s_flush_done, 0);
    ESP_ERROR_CHECK(esp_lcd_panel_draw_bitmap(bsp_display_panel(),
                                              0, y, UI_SCREEN_W, y + h,
                                              s_stripe));
    if (xSemaphoreTake(s_flush_done, pdMS_TO_TICKS(1000)) != pdTRUE) {
        ESP_LOGE(TAG, "第 %d 条 SPI 传输超时", y / STRIPE_H);
    }
}

/* 画一帧。分层由会话层决定：弹层是盖在战斗界面上的，不能自己猜。 */
static void draw_frame(void)
{
    BattleScreen bs;
    MenuScreen   ms;
    memset(&bs, 0, sizeof(bs));
    memset(&ms, 0, sizeof(ms));

    ses_view(&s_ses, &bs);
    ses_menu_view(&s_ses, &ms);
    const SesLayer layer = ses_layer(&s_ses);

    for (int i = 0; i < STRIPES; i++) {
        const int y = i * STRIPE_H;
        const int h = (UI_SCREEN_H - y < STRIPE_H) ? UI_SCREEN_H - y : STRIPE_H;

        Surface s;
        r_init(&s, s_stripe, UI_SCREEN_W, h, UI_SCREEN_W);
        r_set_origin(&s, 0, y);
        r_fill(&s, 0);   /* 先涂黑：漏画的地方在真机上一眼能看见 */

        switch (layer) {
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

        flush_stripe(y, h);
    }
}

/* ---------------------------------------------------------------- 刷新指纹 */

typedef struct {
    uint8_t  phase;
    uint8_t  screen;
    int16_t  cursor;
    uint32_t log_index;
    int8_t   wait_anim;
    uint16_t turn;
} UiStamp;

static UiStamp ui_stamp(void)
{
    UiStamp st;
    st.phase     = (uint8_t)ses_phase(&s_ses);
    st.screen    = (uint8_t)ui_state_screen(&s_ses.ui);
    st.cursor    = (int16_t)ui_state_cursor(&s_ses.ui);
    st.log_index = ses_log_index(&s_ses);
    st.wait_anim = s_ses.wait_anim;
    st.turn      = s_ses.b.turn;
    return st;
}

static bool stamp_differs(const UiStamp *a, const UiStamp *b)
{
    return a->phase != b->phase || a->screen != b->screen ||
           a->cursor != b->cursor || a->log_index != b->log_index ||
           a->wait_anim != b->wait_anim || a->turn != b->turn;
}

/* ---------------------------------------------------------------- main */

void app_main(void)
{
    ESP_ERROR_CHECK(bsp_display_init());
    bsp_display_backlight(100);
    ESP_ERROR_CHECK(bsp_button_init());

    s_flush_done = xSemaphoreCreateBinary();
    if (!s_flush_done) {
        ESP_LOGE(TAG, "创建刷屏信号量失败");
        abort();
    }
    const esp_lcd_panel_io_callbacks_t cbs = {
        .on_color_trans_done = on_color_trans_done,
    };
    ESP_ERROR_CHECK(esp_lcd_panel_io_register_event_callbacks(bsp_display_io(),
                                                                &cbs, NULL));

    /* 真机上每一局都不同；赛后重开由会话层自己换队。 */
    *battle_rng_state() = esp_random() | 1u;

    ses_init(&s_ses, (uint8_t)AI_SMART);
    ses_set_fonts(&s_ses, &kFonts);

    draw_frame();
    UiStamp prev = ui_stamp();

    uint32_t fed_ms = now_ms();
    uint32_t last_tick = fed_ms;

    while (true) {
        uint32_t now = now_ms();

        if (now - fed_ms > CATCHUP_LIMIT_MS) {
            fed_ms = now - CATCHUP_LIMIT_MS;
        }
        while (fed_ms < now) {
            /* ADC 读失败就是 -1，驱动会当「未按下」，不会误判成上键 */
            ses_poll(&s_ses, fed_ms, bsp_button_read_mv());
            fed_ms++;
        }

        if ((uint32_t)(now - last_tick) >= TICK_MS) {
            ses_tick(&s_ses, now);
            last_tick = now;
        }

        UiStamp cur = ui_stamp();
        if (stamp_differs(&cur, &prev)) {
            draw_frame();
            prev = cur;
        }

        vTaskDelay(pdMS_TO_TICKS(1));
    }
}
