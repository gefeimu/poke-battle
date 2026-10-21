# 生成的数据表（gen/）作为一个独立组件。
# 不放进 components/：这里的东西全是 tools/ 生成出来的，与手写代码分开更好找。
idf_component_register(
    SRCS "poke_moves.c"
         "poke_species.c"
         "poke_types.c"
         "poke_abilities.c"
         "poke_items.c"
         "poke_natures.c"
         "poke_learnsets.c"
         "poke_sprites.c"
         "poke_icons.c"
         "poke_font_data.c"
    INCLUDE_DIRS "."
    PRIV_INCLUDE_DIRS "../src"
)
