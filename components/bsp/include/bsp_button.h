// components/bsp/include/bsp_button.h
// 三键共用一个 ADC 引脚,靠分压电阻区分。电压窗口见 bsp_pins.h。
//
// 与官方仓的差异(本项目只取采样这一半)
// ------------------------------------
// 官方 bsp_button_init() 把 iot_button 包了一层,回调跑在共享 esp_timer 任务里。
// 本项目的按键时序在 src/poke_input.c:只吃「一个 mV 读数 + 一个毫秒时基」,
// 全部能在主机侧断言(去抖 / 40ms 按压确认 / 500ms 长按 / 键位锁)。
// 所以这里删掉了事件枚举、回调、deep sleep 辅助,只留 ADC 初始化与读数。
// 引脚与电压窗口仍以 bsp_pins.h 为单一事实来源。
#pragma once

#include "esp_err.h"

// 创建 ADC1 oneshot 单元与曲线拟合校准。成功后可重复调用。
// 失败会回滚本次已创建的资源,修正故障后可重试。
esp_err_t bsp_button_init(void);

// 读当前 ADC 校准后的电压(mV)。松开约 3300;读取失败返回 -1。
int bsp_button_read_mv(void);
