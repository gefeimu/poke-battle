/*
 * test_sprites.c - 精灵图数据自检
 *
 * 精灵图的 bug 和招式表不一样：招式表错了会崩或打错伤害，
 * 而精灵图错了**照样能编译、能跑、只是画出来是碎噪点**。
 * 之前就发生过：pack_4bpp 名字和注释都写 4bpp，代码却是
 * `(v & 0x03) << (6-k*2)`（真正的 2bpp），量化好的 16 色
 * 被砍到4 色，427 张图全部无法辨认，却没人发现——
 * 因为C 文件头注释也是错的（与代码一致），而渲染层还没写。
 *
 * 所以这一组断言专门盯「安静地错」的形态：
 *   1. 尺寸 / 张数 / 每图字节数
 *   2. 解码后 16 色调色板是否真的被用到（4 色化会在这里暴露）
 *   3. 覆盖率是否合理（太小 = 图几乎全透明，太大 = 全是噪点）
 *   4. 三代图与 Mega 图的像素规格是否一致
 *
 * 肉眼验证仍然必要，跑 `make sprites-preview` 导出 PNG 看。
 */
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "poke_species.h"
#include "poke_icons.h"
#include "poke_sprites.h"

static int g_pass = 0, g_fail = 0;

static void check(int cond, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    char buf[160];
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

/*
 * 按 4bpp 解码像素。**这条公式必须与
 * tools/build_sprites_c.py::pack_4bpp 完全一致**。
 * 两边曾不一致过（生成 2bpp / 消费 2bpp 但调色板 16 色），
 * 结果是图全毁。任何一边改动都必须同步。
 */
static uint8_t px4bpp(const SpriteEntry *e, int i)
{
    return (uint8_t)((e->data[i / 2] >> ((i % 2) ? 0 : 4)) & 0xF);
}

/* 单图统计：返回 1 表示数据合理 */
static void inspect(const SpriteEntry *e, int total_px,
                    int *used_colors, int *opaque_px)
{
    int used_mask = 0;
    int opaque = 0;
    for (int i = 0; i < total_px; i++) {
        uint8_t v = px4bpp(e, i);
        if (v) {
            used_mask |= 1 << v;
            opaque++;
        }
    }
    int colors = 0;
    for (int b = 0; b < 16; b++) {
        if (used_mask & (1 << b)) {
            colors++;
        }
    }
    *used_colors = colors;
    *opaque_px = opaque;
}

int main(void)
{
    printf("=== 精灵图数据自检 ===\n\n");

    /* ---------- 1. 基本规格 ---------- */
    printf("[规格] 尺寸与张数\n");
    int size_ok = 1;
    int front_n = 0, back_n = 0, shiny_n = 0, mega_n = 0;
    for (int i = 0; i < SPRITE_COUNT; i++) {
        const SpriteEntry *e = &kSpriteTable[i];
        if (e->w != SPRITE_SIZE || e->h != SPRITE_SIZE) {
            size_ok = 0;
        }
        switch (e->kind) {
            case SPR_FRONT: front_n++; break;
            case SPR_BACK:  back_n++;  break;
            case SPR_SHINY: shiny_n++; break;
            case SPR_MEGA:  mega_n++;  break;
            default: break;
        }
    }
    check(size_ok, "全部 %d 张图都是 %dx%d", SPRITE_COUNT, SPRITE_SIZE,
          SPRITE_SIZE);
    check(front_n == 386, "正面图 386 张，实际 %d", front_n);
    check(mega_n == 41, "Mega 图 41 张，实际 %d", mega_n);
    check(back_n == 0 && shiny_n == 0,
          "不含背面/闪亮（背面 %d、闪亮 %d）", back_n, shiny_n);

    /* 每图字节数 = w*h/2（4bpp = 每字节 2 像素） */
    check(SPRITE_SIZE * SPRITE_SIZE / 2 == 2048,
          "每图字节数应为 %d*%d/2 = %d", SPRITE_SIZE, SPRITE_SIZE,
          SPRITE_SIZE * SPRITE_SIZE / 2);

    /* ---------- 2. 覆盖全部三代物种 ---------- */
    printf("\n[覆盖] 每个 pid 都能查到正面图\n");
    int missing = 0;
    for (uint16_t pid = 1; pid <= 386; pid++) {
        if (!sprite_get(pid, SPR_FRONT)) {
            missing++;
        }
    }
    check(missing == 0, "386 个 pid 全部有正面图，缺 %d 个", missing);

    int mega_missing = 0, mega_n2 = 0;
    for (int i = 0; i < MEGA_COUNT; i++) {
        if (!sprite_get(kMegaMap[i].mega_pid, SPR_MEGA)) {
            mega_missing++;
        }
        mega_n2++;
    }
    check(mega_missing == 0, "%d 个 Mega 形态全部有图，缺 %d 个", mega_n2,
          mega_missing);

    /* 查不存在的应返回 NULL */
    check(sprite_get(9999, SPR_FRONT) == NULL,
          "查不存在的 pid 返回 NULL");
    check(sprite_get(1, SPR_BACK) == NULL, "未导入的背面返回 NULL");

    /* ---------- 3. 调色板真的被用到（防 4 色化） ---------- */
    printf("\n[色深] 16 色调色板必须真的被用到\n");
    /*
     * 这是最关键的一条。pack_4bpp 曾经写成 2bpp，把 16 色砍成 4 色，
     * 而这里如果只查「非透明像素数」，退化后的图也能蒙混过关——
     * 必须统计**用到了几个不同索引**。正常图至少 4 色，复杂的多彩图
     * 能用到 10色以上。
     */
    int too_few = 0, worst_colors = 99;
    int worst_pid = 0;
    const int total_px = SPRITE_SIZE * SPRITE_SIZE;
    for (int i = 0; i < SPRITE_COUNT; i++) {
        const SpriteEntry *e = &kSpriteTable[i];
        int colors = 0, opaque = 0;
        inspect(e, total_px, &colors, &opaque);
        if (colors < 4) {
            too_few++;
            if (colors < worst_colors) {
                worst_colors = colors;
                worst_pid = e->id;
            }
        }
    }
    if (too_few) {
        check(0, "有 %d 张图只用不到 4 色，最少的是 pid=%d 的 %d 色",
              too_few, worst_pid, worst_colors);
    } else {
        check(1, "427 张图全部用到 >= 4 色（无 4bpp 被误降成 2bpp）");
    }

    /* ---------- 4. 覆盖率合理 ---------- */
    printf("\n[覆盖率] 非透明像素占比应在合理区间\n");
    /*
     * 64×64 战斗立绘的覆盖率实测分布：8% ~ 66%，中位 29%。
     * 低的那批不是故障——三代图鉴里体型小的宝可梦官方原图就小
     * （如 pid175托ge皮原图只有 22×25 像素，占画布 7%）。
     * 所以下界卡在 4%：低于它说明图几乎全透明，是缩放/裁剪真坏了。
     * 上界卡 85%：高于它说明图几乎铺满，多半是尺寸没缩放对。
     */
    int out_low = 0, out_high = 0;
    int min_pct = 1000, max_pct = 0;
    int min_pid = 0;
    for (int i = 0; i < SPRITE_COUNT; i++) {
        const SpriteEntry *e = &kSpriteTable[i];
        int colors = 0, opaque = 0;
        inspect(e, total_px, &colors, &opaque);
        int pct = opaque * 100 / total_px;
        if (pct < min_pct) {
            min_pct = pct;
            min_pid = e->id;
        }
        if (pct > max_pct) {
            max_pct = pct;
        }
        if (pct < 4) {
            out_low++;
        }
        if (pct > 85) {
            out_high++;
        }
    }
    printf("  覆盖率区间 %d%%(pid=%u) ~ %d%%，中位约 29%%\n", min_pct,
           min_pid, max_pct);
    check(out_low == 0, "无图几近全透明（<4%%，%d 张）", out_low);
    check(out_high == 0, "无图几乎铺满（>85%%，%d 张）", out_high);

    /* ---------- 5. 三代正面与 Mega 像素规格一致 ---------- */
    printf("\n[规格统一] 正面与 Mega 同为 %dx%d", SPRITE_SIZE, SPRITE_SIZE);
    printf("\n");
    int spec_ok = 1;
    for (int i = 0; i < SPRITE_COUNT; i++) {
        const SpriteEntry *e = &kSpriteTable[i];
        if (e->w != SPRITE_SIZE || e->h != SPRITE_SIZE) {
            spec_ok = 0;
        }
    }
    check(spec_ok, "正面与 Mega 像素规格统一（用户要求）");

    /* ---------- 6. 调色板自身合法 ---------- */
    printf("\n[调色板] 索引 0 必须为透明占位\n");
    int pal_bad = 0;
    for (int i = 0; i < SPRITE_COUNT; i++) {
        if (kSpriteTable[i].palette == NULL) {
            pal_bad++;
        }
    }
    check(pal_bad == 0, "全部图都带调色板，缺失 %d 张", pal_bad);

    /* ---------- 7. 图标表（32x32 队伍小图标） ---------- */
    /*
     * 图标是独立的 kIconTable（不复用 kSpriteTable、不影响 SPRITE_COUNT）。
     * 这里只盯图标自身：张数、尺寸、覆盖 1..386、越界返回 NULL、
     * 像素非空，以及 id 是否严格递增（表损坏会在这里暴露）。
     */
    printf("\n[图标] 32x32 队伍小图标表\n");
    check(ICON_COUNT == 386, "ICON_COUNT == 386（实际 %d）", ICON_COUNT);

    int icon_missing = 0, icon_badsize = 0;
    for (uint16_t pid = 1; pid <= 386; pid++) {
        const SpriteEntry *e = icon_get(pid);
        if (!e) {
            icon_missing++;
        } else if (e->w != 32 || e->h != 32) {
            icon_badsize++;
        }
    }
    check(icon_missing == 0, "386 个 pid 全部查得到图标，缺 %d 个", icon_missing);
    check(icon_badsize == 0, "全部图标尺寸都是 32x32，尺寸不对 %d 个", icon_badsize);

    check(icon_get(0) == NULL, "icon_get(0) 返回 NULL");
    check(icon_get(387) == NULL, "icon_get(387) 返回 NULL");
    check(icon_get(9999) == NULL, "icon_get(9999) 返回 NULL");

    /* 抽样若干图标：像素数据里既要有非透明像素，也不能整张不透明 */
    static const uint16_t sample[] = { 1, 25, 386 };
    int sample_bad = 0;
    for (unsigned k = 0; k < sizeof(sample) / sizeof(sample[0]); k++) {
        const SpriteEntry *e = icon_get(sample[k]);
        if (!e) {
            sample_bad++;
            continue;
        }
        int opaque = 0;
        for (int i = 0; i < 32 * 32; i++) {
            if (px4bpp(e, i)) {
                opaque++;
            }
        }
        if (opaque == 0 || opaque == 32 * 32) {
            sample_bad++;
        }
    }
    check(sample_bad == 0, "抽样图标（1/25/386）像素非空且非全不透明，异常 %d 个",
          sample_bad);

    /* id 严格递增，且首尾分别是 1 与 386 */
    int order_ok = 1;
    for (int i = 1; i < ICON_COUNT; i++) {
        if (kIconTable[i].id != kIconTable[i - 1].id + 1) {
            order_ok = 0;
        }
    }
    check(order_ok, "kIconTable 的 id 严格递增");
    check(kIconTable[0].id == 1, "首项 id == 1（实际 %u）", kIconTable[0].id);
    check(kIconTable[ICON_COUNT - 1].id == 386, "末项 id == 386（实际 %u）",
          kIconTable[ICON_COUNT - 1].id);

    printf("\n=== 结果: %d 通过, %d 失败 ===\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}