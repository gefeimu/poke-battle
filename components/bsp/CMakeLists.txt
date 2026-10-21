# components/bsp —— 官方 FoloToy ai-passport 的 BSP（MIT），只留本项目用到的部分。
#
# 与上游的差异见 THIRD-PARTY-NOTICES.md：
#   - 保留 src/bsp_display.c 与 include/bsp_pins.h / bsp_display.h
#   - 按键只留 ADC 采样（bsp_button.h/.c 裁剪说明见头文件），不引入 iot_button
#   - 不引入 LVGL / 音频 / 电量计：本项目没有这些层
# 因此 REQUIRES 也从上游的一长串收敛到 driver / esp_lcd / esp_adc。
idf_component_register(
    SRCS "src/bsp_display.c"
         "src/bsp_button.c"
    INCLUDE_DIRS "include"
    REQUIRES driver esp_lcd esp_adc
)
