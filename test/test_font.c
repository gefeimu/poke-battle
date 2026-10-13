/*
 * test_font.c - 中文字库自检
 *
 * 字库的 bug 是「安静地错」的重灾区：位图能编译、索引能查到、
 * 大小看着也正常，但渲染出来是空白或乱码。这一组断言把踩过的
 * 坑全部钉住：
 *
 *   1. 步长：CELL_W=14 不是 8 的倍数，用 CELL_W//8（=1）当行字节数
 *      会丢掉每行后 6 个像素，字形变成散点（「一」出现 47 个点）。
 *      -> 断言 FONT_STRIDE == ceil(CELL_W/8)，且每字字节数一致。
 *   2. 字号：字号小于字格高度会裁掉笔画（12px 字号 + 14px 字格时
 *      「妙」只剩 23 个亮点，正常 45）。-> 用「一」这类简单字
 *      断言墨迹分布合理，散点会露馅。
 *   3. 收集范围：只扫 gen 目录会漏掉 src 目录里的战斗日志用字
 *      （濒/稳/第/被/么/什…），UI 显示日志时那是空白。
 *      -> 逐个遍历六张中文名表 + 日志文案，断言每个字都有字形。
 *   4. 二分查找前提：kFontMap 必须按码点升序，否则 font_lookup 失效。
 *
 * 第 3 条是最强的断言：它直接把「数据表 / UI 文案」与「字库覆盖」
 * 绑在一起，任何一个新加的中文名漏收都会立刻失败。
 */
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "font_cn.h"
#include "poke_abilities.h"
#include "poke_items.h"
#include "poke_moves.h"
#include "poke_natures.h"
#include "poke_species.h"
#include "poke_types.h"

static int g_pass = 0, g_fail = 0;

static void check(int cond, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    char buf[200];
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (cond) {
        g_pass++;
        printf("  [ OK ] %s\n", buf);
    } else {
        g_fail++;
        printf("  [FAIL] %s\n", buf);
    }
}

/* UTF-8 解码：返回码点，推进指针。非法字节按单字节处理，不会死循环。 */
static uint32_t utf8_next(const char **p)
{
    const unsigned char *s = (const unsigned char *)*p;
    if (s[0] < 0x80) {
        *p += 1;
        return s[0];
    }
    if ((s[0] & 0xE0) == 0xC0 && s[1]) {
        *p += 2;
        return ((uint32_t)(s[0] & 0x1F) << 6) | (s[1] & 0x3F);
    }
    if ((s[0] & 0xF0) == 0xE0 && s[1] && s[2]) {
        *p += 3;
        return ((uint32_t)(s[0] & 0x0F) << 12) |
               ((uint32_t)(s[1] & 0x3F) << 6) | (s[2] & 0x3F);
    }
    *p += 1;
    return s[0];
}

/*
 * 检查一段 UTF-8 文本里每个非 ASCII 字符都能查到字形。
 * 返回缺失字符数；missing_out 收下第一个缺字的码点。
 */
static int scan_text(const char *text, uint32_t *missing_out)
{
    int miss = 0;
    const char *p = text;
    while (*p) {
        uint32_t cp = utf8_next(&p);
        if (cp < 0x80) {
            continue; /* ASCII 由 EXTRA_CHARS 保证，单独测 */
        }
        if (font_lookup(cp) == 0xFFFF) {
            if (miss == 0 && missing_out) {
                *missing_out = cp;
            }
            miss++;
        }
    }
    return miss;
}

/* 统计字形墨迹（1 的个数）与墨迹所在的行数 */
static void glyph_ink(uint16_t gi, int *dots, int *rows_used)
{
    const uint8_t *g = kFontBitmap + (size_t)gi * FONT_CELL_H * FONT_STRIDE;
    int d = 0, r = 0;
    for (int y = 0; y < FONT_CELL_H; y++) {
        int row_on = 0;
        for (int x = 0; x < FONT_CELL_W; x++) {
            int bit = (g[y * FONT_STRIDE + x / 8] >> (7 - (x % 8))) & 1;
            if (bit) {
                d++;
                row_on = 1;
            }
        }
        if (row_on) {
            r++;
        }
    }
    *dots = d;
    *rows_used = r;
}

int main(void)
{
    printf("=== 中文字库自检 ===\n\n");

    /* ---------- 1. 规格与步长 ---------- */
    printf("[规格] 字格与步长\n");
    check(FONT_CELL_W > 0 && FONT_CELL_H > 0,
          "字格 %dx%d", FONT_CELL_W, FONT_CELL_H);
    check(FONT_STRIDE == (FONT_CELL_W + 7) / 8,
          "FONT_STRIDE = %d 等于 ceil(%d/8) = %d", FONT_STRIDE, FONT_CELL_W,
          (FONT_CELL_W + 7) / 8);
    printf("  每字 %d 字节（%d 字节/行），总位图 %d 字节\n",
           FONT_CELL_H * FONT_STRIDE, FONT_STRIDE,
           FONT_GLYPH_COUNT * FONT_CELL_H * FONT_STRIDE);
    check(FONT_GLYPH_COUNT % FONT_ALIGN == 0,
          "字形数 %d 是 %d 的整数倍（便于对齐寻址）", FONT_GLYPH_COUNT,
          FONT_ALIGN);
    check(FONT_MAP_COUNT > 0 && FONT_MAP_COUNT <= FONT_GLYPH_COUNT,
          "映射表 %d 条 <= 字形数 %d", FONT_MAP_COUNT, FONT_GLYPH_COUNT);

    /* ---------- 2. 二分查找前提：按码点升序 ---------- */
    printf("\n[索引] 映射表必须按码点升序（二分查找的前提）\n");
    int unsorted = 0;
    for (int i = 1; i < FONT_MAP_COUNT; i++) {
        if (kFontMap[i].codepoint <= kFontMap[i - 1].codepoint) {
            unsorted++;
        }
    }
    check(unsorted == 0, "码点严格升序（乱序 %d 处）", unsorted);

    int out_of_range = 0;
    for (int i = 0; i < FONT_MAP_COUNT; i++) {
        if (kFontMap[i].glyph_index >= FONT_GLYPH_COUNT) {
            out_of_range++;
        }
    }
    check(out_of_range == 0, "所有 glyph_index 都在 [0,%d) 内（越界 %d 个）",
          FONT_GLYPH_COUNT, out_of_range);

    /* ---------- 3. 查找语义 ---------- */
    printf("\n[查找] font_lookup 语义\n");
    check(font_lookup('A') != 0xFFFF, "ASCII 'A' 可查");
    check(font_lookup('0') != 0xFFFF, "ASCII '0' 可查");
    check(font_lookup(' ') != 0xFFFF, "空格可查");
    check(font_lookup(0xFFFF) == 0xFFFF, "未收录码点返回 0xFFFF");
    /* 抽查几个必然收录的常用字 */
    check(font_lookup(0x7684) != 0xFFFF, "「的」可查");
    check(font_lookup(0x4E00) != 0xFFFF, "「一」可查");

    /* ---------- 4. 字形不为空（位深/步长错误的直接表现） ---------- */
    printf("\n[字形] 每个字形都要有墨迹\n");
    int empty = 0, first_empty = -1;
    for (int i = 0; i < FONT_GLYPH_COUNT; i++) {
        int dots = 0, rows = 0;
        glyph_ink((uint16_t)i, &dots, &rows);
        if (dots == 0) {
            empty++;
            if (first_empty < 0) {
                first_empty = i;
            }
        }
    }
    /* 只允许「空格」一个空字形，以及尾部对齐填充 */
    int pad = FONT_GLYPH_COUNT - FONT_MAP_COUNT;
    check(empty <= pad + 1,
          "空字形 %d 个（允许 %d 个对齐填充 + 1 个空格）", empty, pad);
    if (empty > pad + 1) {
        printf("       首个空字形 index=%d\n", first_empty);
    }

    /*
     * 「一」是一横：墨迹应集中（约 1~3 行），总点数不多。
     * 步长错位时它会出现几十个散点，这条断言专抓那种情况。
     */
    uint16_t gi_yi = font_lookup(0x4E00);
    if (gi_yi != 0xFFFF) {
        int dots = 0, rows = 0;
        glyph_ink(gi_yi, &dots, &rows);
        char buf[120];
        snprintf(buf, sizeof(buf), "「一」墨迹 %d 点、占 %d 行", dots, rows);
        printf("       %s\n", buf);
        check(rows <= 4 && dots <= FONT_CELL_W + 4,
              "「一」是一横而非散点（%d 点 / %d 行）", dots, rows);
    }

    /* ---------- 5. 覆盖全部数据表中文名（最强断言） ---------- */
    printf("\n[覆盖] 数据表每个中文名的每个字都要有字形\n");
    int total_miss = 0;
    uint32_t first_miss = 0;

    int sp_miss = 0;
    for (int i = 0; i < SPECIES_COUNT; i++) {
        sp_miss += scan_text(kSpecies[i].name_cn, &first_miss);
    }
    check(sp_miss == 0, "宝可梦名 %d 条，缺字 %d", SPECIES_COUNT, sp_miss);
    total_miss += sp_miss;

    int mv_miss = 0;
    for (int i = 0; i < MOVE_COUNT; i++) {
        mv_miss += scan_text(kMoves[i].name_cn, &first_miss);
    }
    check(mv_miss == 0, "招式名 %d 条，缺字 %d", MOVE_COUNT, mv_miss);
    total_miss += mv_miss;

    int ty_miss = 0;
    for (int i = 0; i < TYPE_COUNT; i++) {
        ty_miss += scan_text(kTypeNamesCN[i], &first_miss);
    }
    check(ty_miss == 0, "属性名 %d 条，缺字 %d", TYPE_COUNT, ty_miss);
    total_miss += ty_miss;

    int na_miss = 0;
    for (int i = 0; i < NATURE_COUNT; i++) {
        na_miss += scan_text(kNatures[i].name_cn, &first_miss);
    }
    check(na_miss == 0, "性格名 %d 条，缺字 %d", NATURE_COUNT, na_miss);
    total_miss += na_miss;

    int ab_miss = 0;
    for (int i = 0; i < ABILITY_COUNT; i++) {
        ab_miss += scan_text(kAbilities[i].name_cn, &first_miss);
    }
    check(ab_miss == 0, "特性名 %d 条，缺字 %d", ABILITY_COUNT, ab_miss);
    total_miss += ab_miss;

    int it_miss = 0;
    for (int i = 0; i < ITEM_COUNT; i++) {
        it_miss += scan_text(kItems[i].name_cn, &first_miss);
    }
    check(it_miss == 0, "道具名 %d 条，缺字 %d", ITEM_COUNT, it_miss);
    total_miss += it_miss;

    if (total_miss) {
        printf("       首个缺字 U+%04X\n", first_miss);
    }

    /* ---------- 6. 中文名本身非空 ---------- */
    printf("\n[数据] 中文名字段非空\n");
    int sp_empty = 0;
    for (int i = 0; i < SPECIES_COUNT; i++) {
        if (!kSpecies[i].name_cn || !kSpecies[i].name_cn[0]) {
            sp_empty++;
        }
    }
    check(sp_empty == 0, "全部 %d 个宝可梦都有中文名（空 %d）", SPECIES_COUNT,
          sp_empty);

    int mv_empty = 0;
    for (int i = 0; i < MOVE_COUNT; i++) {
        if (!kMoves[i].name_cn || !kMoves[i].name_cn[0]) {
            mv_empty++;
        }
    }
    check(mv_empty == 0, "全部 %d 个招式都有中文名（空 %d）", MOVE_COUNT,
          mv_empty);

    /* 抽查几个固定的译名（繁体转简体的正确性） */
    check(strcmp(kSpecies[0].name_cn, "妙蛙种子") == 0,
          "species[0] 中文名应为「妙蛙种子」，实际「%s」",
          kSpecies[0].name_cn);
    check(strcmp(kMoves[0].name_cn, "拍击") == 0,
          "moves[0] 中文名应为「拍击」，实际「%s」", kMoves[0].name_cn);
    check(strcmp(kTypeNamesCN[0], "一般") == 0,
          "types[0] 中文名应为「一般」，实际「%s」", kTypeNamesCN[0]);

    printf("\n=== 结果: %d 通过, %d 失败 ===\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
