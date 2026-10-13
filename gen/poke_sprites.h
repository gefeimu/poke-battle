/* 由 tools/build_sprites_c.py 自动生成 */
#ifndef POKE_SPRITES_H
#define POKE_SPRITES_H

#include <stdint.h>
#include <stdbool.h>

/* 调色板槽位：索引 0 = 透明 */
#define PALETTE_SIZE 16
#define PAL_TRANSPARENT 0

#define SPRITE_SIZE 64

typedef enum {
    SPR_FRONT = 0,
    SPR_BACK  = 1,
    SPR_SHINY = 2,
    SPR_MEGA  = 3,
    SPR_MEGA_BACK = 4,
} SpriteKind;

typedef struct {
    uint16_t id;              /* pokemon id */
    uint8_t kind;             /* SpriteKind */
    const uint8_t *data;      /* 4bpp 像素，64*64/2 字节 */
    const uint16_t *palette;  /* RGB565，长度 PALETTE_SIZE */
    uint8_t w, h;
} SpriteEntry;

#define SPRITE_COUNT 427
extern const SpriteEntry kSpriteTable[SPRITE_COUNT];

/* 按 pokemon id + 图种查找，找不到返回 NULL */
const SpriteEntry *sprite_get(uint16_t pid, uint8_t kind);

#endif /* POKE_SPRITES_H */
