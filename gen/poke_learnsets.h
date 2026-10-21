/* 由 tools/gen_gen3_data.py 自动生成 */
#ifndef POKE_LEARNSETS_H
#define POKE_LEARNSETS_H

#include <stdint.h>
#include <stddef.h>

#define LEARNSET_MAX_NODES 24
typedef struct { uint8_t level; uint16_t move_id; } LearnEntry;

typedef struct {
    uint8_t node_count;
    LearnEntry nodes[LEARNSET_MAX_NODES];
} LearnSet;

/* 检索用行结构：pid + 该 pid 的学习表 */
typedef struct { uint16_t pid; LearnSet set; } LearnSetRow;

#define LEARNSET_COUNT 427
extern const LearnSetRow kLearnSets[LEARNSET_COUNT];

/* 按 pokemon_id 查学习表，找不到返回 NULL */
const LearnSet *learnset_get(uint16_t pokemon_id);

#endif