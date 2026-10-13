/* 由 tools/gen_gen3_data.py 自动生成 */
#ifndef POKE_TYPES_H
#define POKE_TYPES_H

#include <stdint.h>

typedef enum {
    TYPE_NORMAL = 0,
    TYPE_FIGHTING = 1,
    TYPE_FLYING = 2,
    TYPE_POISON = 3,
    TYPE_GROUND = 4,
    TYPE_ROCK = 5,
    TYPE_BUG = 6,
    TYPE_GHOST = 7,
    TYPE_STEEL = 8,
    TYPE_FIRE = 9,
    TYPE_WATER = 10,
    TYPE_GRASS = 11,
    TYPE_ELECTRIC = 12,
    TYPE_PSYCHIC = 13,
    TYPE_ICE = 14,
    TYPE_DRAGON = 15,
    TYPE_DARK = 16,
    TYPE_FAIRY = 17,
    TYPE_COUNT
} PokeType;

#define TYPE_MATRIX_SIZE 18

/* [攻方][守方]，值 = 倍率 * 10（20=2x, 10=1x, 5=0.5x, 0=0x） */
extern const uint8_t kTypeEffect[TYPE_MATRIX_SIZE][TYPE_MATRIX_SIZE];
extern const char *const kTypeNames[TYPE_COUNT];

/* 简体中文属性名（索引与 TYPE_* 一致），UI 显示用 */
extern const char *const kTypeNamesCN[TYPE_COUNT];

#endif
