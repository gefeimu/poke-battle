# 固件入口。只有 app_main.c：所有游戏逻辑都在 src/ 的组件里，
# 这里只做「真机接线」——面板、ADC 按键、条带刷屏与主循环。
idf_component_register(
    SRCS "app_main.c"
    INCLUDE_DIRS "."
    REQUIRES bsp src
    PRIV_REQUIRES esp_timer esp_lcd log freertos esp_hw_support
)
