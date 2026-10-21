/* 由 tools/gen_gen3_data.py 自动生成 */
#ifndef POKE_SPECIES_H
#define POKE_SPECIES_H

#include <stdint.h>
#include <stddef.h>
#include "poke_types.h"

typedef struct {
    uint16_t id;        /* pokemon id，1..386，Mega 段 10034..10134 */
    uint16_t species_id;
    const char *name;
    const char *name_cn;   /* 简体中文名，UI 显示用 */
    uint8_t gen;         /* 1..3 */
    uint8_t is_mega;
    uint8_t type1, type2;
    uint8_t hp, atk, def, spa, spd, spe;   /* 种族值 */
    uint8_t catch_rate;
    uint8_t growth_rate;
    int8_t  gender_rate;
} SpeciesDef;

#define SPECIES_COUNT 427
extern const SpeciesDef kSpecies[SPECIES_COUNT];

/* Mega -> 基础形态 species_id 映射表 */
#define MEGA_COUNT 41
typedef struct { uint16_t mega_pid; uint16_t base_species_id; } MegaMap;
extern const MegaMap kMegaMap[MEGA_COUNT];

const SpeciesDef *species_get(uint16_t pokemon_id);
uint16_t mega_to_base(uint16_t pokemon_id);

#endif