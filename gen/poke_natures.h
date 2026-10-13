/* 由 tools/gen_gen3_data.py 自动生成 */
#ifndef POKE_NATURES_H
#define POKE_NATURES_H

#include <stdint.h>

typedef struct {
    uint8_t increased_stat;  /* 0=无, 1=hp 2=atk 3=def 4=spa 5=spd 6=spe */
    uint8_t decreased_stat;
    const char *name;
    const char *name_cn;   /* 简体中文名，UI 显示用 */
} NatureDef;

#define NATURE_COUNT 26
extern const NatureDef kNatures[NATURE_COUNT];

#endif