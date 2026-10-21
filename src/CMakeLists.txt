# 引擎 + 会话 + 输入 + 渲染，作为独立组件。
# 这一份清单必须与 Makefile 的 SCREEN_SRC 保持一致（同一份源码，两条构建路径）。
idf_component_register(
    SRCS "poke_battle.c"
         "poke_ability.c"
         "poke_item.c"
         "poke_calc.c"
         "poke_ai.c"
         "poke_session.c"
         "poke_ui_state.c"
         "poke_input.c"
         "poke_render.c"
         "poke_font.c"
         "poke_ui.c"
    INCLUDE_DIRS "."
    REQUIRES gen
)
