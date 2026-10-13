/*
 * dump_sprites.c - 把编译进固件的精灵图导出成 PNG，用肉眼确认画风
 *
 * 用途：本项目的精灵图来源在 README / 生成脚本注释 / C 文件头注释
 * 三处说法不一致过（ruby-sapphire?firered-leafgreen?black-white?），
 * 光看代码无法判断，只能把图渲染出来看。
 *
 * 用法：
 *   ./dump_sprites                                 默认 10 只，棋盘底
 *   ./dump_sprites --alpha 6 25 150               指定 pid，透明底
 *   ./dump_sprites --alpha --out docs/img 6 mega:10034
 *                                                 含 Mega 形态
 *   ./dump_sprites --help
 *
 * 默认输出 data/sprites_preview/pid{N}.png ；`mega:N` 输出 mega{N}.png
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "poke_sprites.h"
#include "poke_species.h"

/* ---- 极简 PNG 写出（支持 RGB888 与 RGBA8888，无压缩库依赖） ---- */

static uint32_t crc_table[256];
static int crc_ready = 0;

static void crc_init(void)
{
    for (uint32_t n = 0; n < 256; n++) {
        uint32_t c = n;
        for (int k = 0; k < 8; k++) {
            c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : (c >> 1);
        }
        crc_table[n] = c;
    }
    crc_ready = 1;
}

static uint32_t crc_update(uint32_t crc, const uint8_t *buf, size_t len)
{
    for (size_t i = 0; i < len; i++) {
        crc = crc_table[(crc ^ buf[i]) & 0xFF] ^ (crc >> 8);
    }
    return crc;
}

static void put32be(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

static void chunk(FILE *f, const char *type, const uint8_t *data, size_t len)
{
    uint8_t hdr[8];
    put32be(hdr, (uint32_t)len);
    memcpy(hdr + 4, type, 4);
    fwrite(hdr, 1, 8, f);
    if (len) {
        fwrite(data, 1, len, f);
    }
    uint32_t crc = 0xFFFFFFFFu;
    crc = crc_update(crc, (const uint8_t *)type, 4);
    crc = crc_update(crc, data, len);
    crc ^= 0xFFFFFFFFu;
    uint8_t cb[4];
    put32be(cb, crc);
    fwrite(cb, 1, 4, f);
}

/*
 * color_type: 2 = RGB888, 6 = RGBA8888
 *
 * zlib 用的是「存储」块（不压缩）。**单个 stored 块的数据长度上限是
 * 65535 字节**（长度字段只有 2 字节），超过必须切块，否则长度字段回绕、
 * 写出损坏的 PNG。早前写字体预览图时正是踩了这个坑（图更大，一行就超了）。
 */
static void write_png(const char *path, const uint8_t *px, int w, int h,
                      int color_type)
{
    if (!crc_ready) {
        crc_init();
    }
    FILE *f = fopen(path, "wb");
    if (!f) {
        printf("  !! 无法写入 %s\n", path);
        return;
    }
    static const uint8_t sig[8] = { 137, 'P', 'N', 'G', 13, 10, 26, 10 };
    fwrite(sig, 1, 8, f);

    int bpp = (color_type == 6) ? 4 : 3;
    uint8_t ihdr[13];
    put32be(ihdr, (uint32_t)w);
    put32be(ihdr + 4, (uint32_t)h);
    ihdr[8] = 8;                    /* bit depth */
    ihdr[9] = (uint8_t)color_type;
    ihdr[10] = 0;
    ihdr[11] = 0;
    ihdr[12] = 0;
    chunk(f, "IHDR", ihdr, 13);

    /* 每行前面加一个 filter 字节 0 */
    size_t stride = (size_t)w * (size_t)bpp;
    size_t raw_len = (size_t)h * (stride + 1);
    uint8_t *raw = malloc(raw_len);
    if (!raw) {
        fclose(f);
        return;
    }
    size_t off = 0;
    for (int y = 0; y < h; y++) {
        raw[off++] = 0;
        memcpy(raw + off, px + (size_t)y * stride, stride);
        off += stride;
    }

    /* IDAT 的数据要先攒起来才能算长度，用临时文件式两遍写：
     * 这里直接算 zlib 长度：2 头 + 每块 5 字节 + 数据 + 4 校验 */
    size_t nblocks = (raw_len + 65534) / 65535;
    if (nblocks == 0) {
        nblocks = 1;
    }
    size_t z_len = 2 + nblocks * 5 + raw_len + 4;
    uint8_t *z = malloc(z_len);
    if (!z) {
        free(raw);
        fclose(f);
        return;
    }
    /* 简单起见：把 zlib_store 的输出写进内存缓冲，再作为 IDAT 发出 */
    size_t zi = 0;
    z[zi++] = 0x78;
    z[zi++] = 0x01;
    {
        uint32_t a = 1, b = 0;
        size_t o = 0;
        while (o < raw_len) {
            size_t n = raw_len - o;
            int last = 0;
            if (n > 65535) {
                n = 65535;
            } else {
                last = 1;
            }
            z[zi++] = (uint8_t)(last ? 0x01 : 0x00);
            z[zi++] = (uint8_t)(n & 0xFF);
            z[zi++] = (uint8_t)(n >> 8);
            z[zi++] = (uint8_t)(~n & 0xFF);
            z[zi++] = (uint8_t)((~n >> 8) & 0xFF);
            memcpy(z + zi, raw + o, n);
            for (size_t i = 0; i < n; i++) {
                a = (a + raw[o + i]) % 65521;
                b = (b + a) % 65521;
            }
            zi += n;
            o += n;
        }
        if (raw_len == 0) {
            z[zi++] = 0x01;
            z[zi++] = 0;
            z[zi++] = 0;
            z[zi++] = 0xFF;
            z[zi++] = 0xFF;
        }
        put32be(z + zi, (uint32_t)((b << 16) | a));
        zi += 4;
    }
    chunk(f, "IDAT", z, zi);
    chunk(f, "IEND", NULL, 0);

    free(z);
    free(raw);
    fclose(f);
}

/* RGB565 -> RGB888 */
static void rgb565_to_888(uint16_t c, uint8_t *out)
{
    uint8_t r = (uint8_t)((c >> 11) & 0x1F);
    uint8_t g = (uint8_t)((c >> 5) & 0x3F);
    uint8_t b = (uint8_t)(c & 0x1F);
    /* 5/6/5 位扩到 8 位：v * 255 / 31（或 63） */
    out[0] = (uint8_t)(r * 255 / 31);
    out[1] = (uint8_t)(g * 255 / 63);
    out[2] = (uint8_t)(b * 255 / 31);
}

static int dump_one(uint16_t pid, SpriteKind kind, int alpha, const char *dir)
{
    const SpriteEntry *e = sprite_get(pid, kind);
    if (!e) {
        printf("  pid=%u kind=%d 找不到\n", pid, (int)kind);
        return 0;
    }
    int w = e->w, h = e->h;
    int bpp = alpha ? 4 : 3;
    uint8_t *px = calloc((size_t)w * h * (size_t)bpp, 1);
    if (!px) {
        return 0;
    }
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            int i4 = y * w + x;
            /* 4bpp：每字节 2 像素，高位在前。
             * 注意这与旧的 2bpp 格式不同（每字节 4 像素）——
             * build_sprites_c.py 的 pack_4bpp 曾写成 `& 0x03`，
             * 导致 16 色调色板被砍成 4 色、图变成碎噪点。 */
            uint8_t idx =
                (uint8_t)((e->data[i4 / 2] >> ((i4 % 2) ? 0 : 4)) & 0xF);
            uint8_t *p = px + ((size_t)y * w + x) * (size_t)bpp;
            if (idx == PAL_TRANSPARENT) {
                if (alpha) {
                    p[0] = p[1] = p[2] = 0;
                    p[3] = 0;
                } else {
                    /* 棋盘底，便于看清透明区域与轮廓 */
                    int chk = (((x / 4) + (y / 4)) & 1) ? 210 : 170;
                    p[0] = p[1] = p[2] = (uint8_t)chk;
                }
            } else {
                uint8_t rgb[3];
                rgb565_to_888(e->palette[idx], rgb);
                p[0] = rgb[0];
                p[1] = rgb[1];
                p[2] = rgb[2];
                if (alpha) {
                    p[3] = 255;
                }
            }
        }
    }
    char path[256];
    snprintf(path, sizeof(path), "%s/%s%u.png", dir,
             (kind == SPR_MEGA) ? "mega" : "pid", pid);
    write_png(path, px, w, h, alpha ? 6 : 2);
    const SpeciesDef *sd = species_get(pid);
    printf("  %-10s %-4u %-16s %dx%d -> %s\n",
           (kind == SPR_MEGA) ? "mega" : "pid", pid,
           sd ? sd->name : "?", w, h, path);
    free(px);
    return 1;
}

static void usage(void)
{
    printf(
        "用法: dump_sprites [选项] [pid | mega:pid ...]\n"
        "  --alpha        透明背景（RGBA），默认是棋盘底\n"
        "  --out DIR      输出目录，默认 data/sprites_preview\n"
        "  -h, --help     显示本帮助\n"
        "不带 pid 时导出内置的 10 只示例。\n");
}

int main(int argc, char **argv)
{
    /* 默认样本：初代御三家 + 人气宠，覆盖不同属性配色 */
    static const uint16_t defaults[] = { 1, 4, 6, 9, 25, 65, 130, 150, 212, 103 };
    const int n_default = (int)(sizeof(defaults) / sizeof(defaults[0]));

    int alpha = 0;
    const char *outdir = "data/sprites_preview";
    uint16_t pids[64];
    SpriteKind kinds[64];
    int n_pid = 0;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--alpha")) {
            alpha = 1;
        } else if (!strcmp(argv[i], "--out") && i + 1 < argc) {
            outdir = argv[++i];
        } else if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) {
            usage();
            return 0;
        } else if (!strncmp(argv[i], "mega:", 5)) {
            if (n_pid < 64) {
                pids[n_pid] = (uint16_t)atoi(argv[i] + 5);
                kinds[n_pid] = SPR_MEGA;
                n_pid++;
            }
        } else {
            if (n_pid < 64) {
                pids[n_pid] = (uint16_t)atoi(argv[i]);
                kinds[n_pid] = SPR_FRONT;
                n_pid++;
            }
        }
    }

    printf("导出精灵图...\n");
    int ok = 0;
    if (n_pid == 0) {
        for (int i = 0; i < n_default; i++) {
            ok += dump_one(defaults[i], SPR_FRONT, alpha, outdir);
        }
    } else {
        for (int i = 0; i < n_pid; i++) {
            ok += dump_one(pids[i], kinds[i], alpha, outdir);
        }
    }
    printf("共导出 %d 张\n", ok);
    return ok ? 0 : 1;
}
