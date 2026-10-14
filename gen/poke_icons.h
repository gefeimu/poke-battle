/* 由 tools/build_icons_c.py 自动生成，请勿手工编辑 */
/*
 * 32x32 宝可梦队伍小图标（来源 PokeAPI/sprites 的 generation-iii/icons ——
 * 原生 32x32 火红/叶绿队伍图标，无需缩放，保持原始构图）。
 *
 * 格式与精灵图（poke_sprites.h）相同：
 *   4bpp，每字节 2 像素，高位在前，行优先；
 *   像素 i 的索引 = (data[i/2] >> ((i%2) ? 0 : 4)) & 0xF；
 *   索引 0 = 透明，不绘制。
 *
 * 构成  386 张，每张 512 B 像素 + 16 色 RGB565 调色板
 */
#ifndef POKE_ICONS_H
#define POKE_ICONS_H

#include <stdint.h>
#include "poke_sprites.h"     /* 复用 SpriteEntry */

#define ICON_SIZE  32
#define ICON_COUNT 386

extern const SpriteEntry kIconTable[ICON_COUNT];

/* 按图鉴号取 32x32 队伍小图标；找不到（0 或 >386）返回 NULL */
const SpriteEntry *icon_get(uint16_t pid);

#endif /* POKE_ICONS_H */
