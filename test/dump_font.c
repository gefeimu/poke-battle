/*
 * dump_font.c - 把固件里的点阵字库渲染成图片，用于肉眼验证字形
 *
 * 为什么需要这个工具
 * ------------------
 * 字库的 bug 和精灵图一样是「安静地错」：字号小于字格会让笔画被裁掉
 * （实测 12px 字号 + 14px 字格，「妙」只剩 23 个亮点、正常应有 45 个），
 * 但代码能编译、字库能生成、索引也查得到。只能渲染出来看。
 *
 * 输出 PGM（P5）而非 PNG：PIL 原生支持 PGM，省掉自己写 zlib 编码器
 * （上一版自写 PNG 编码器因存储块超过 65535 字节上限而输出损坏文件）。
 *
 * 中文输入必须走文件
 * ------------------
 * Windows 按**当前 ANSI 代码页**（简体中文是 GBK/CP936）传命令行参数，
 * 不是 UTF-8。直接传中文 argv 会得到错误字节，按 UTF-8 解出的码点查不到
 * 字形，表现为「每个中文都算缺字」—— 而字库其实是好的（曾因此误判
 * 26 个字缺失、白查半天）。所以用 `--file <路径>` 从 UTF-8 文件读，
 * 一行一段文本。
 *
 * 用法：
 *   gcc -Igen -Isrc -o dump_font test/dump_font.c gen/font_cn.c
 *   ./dump_font                      # 渲染内置的示例文本
 *   ./dump_font --file text.txt      # 从 UTF-8 文件读（推荐）
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "font_cn.h"

#define PAD_X 6
#define PAD_Y 6
/* 行距：字格高度 + 2px。中文行距太紧会粘连。 */
#define LINE_GAP 2

/* 解码一个 UTF-8 字符，推进指针。返回码点。 */
static uint32_t next_cp(const char **p)
{
    const unsigned char *s = (const unsigned char *)*p;
    uint32_t cp;
    if (s[0] < 0x80) {
        cp = s[0];
        *p += 1;
    } else if ((s[0] & 0xE0) == 0xC0 && s[1]) {
        cp = ((uint32_t)(s[0] & 0x1F) << 6) | (s[1] & 0x3F);
        *p += 2;
    } else if ((s[0] & 0xF0) == 0xE0 && s[1] && s[2]) {
        cp = ((uint32_t)(s[0] & 0x0F) << 12) |
             ((uint32_t)(s[1] & 0x3F) << 6) | (s[2] & 0x3F);
        *p += 3;
    } else {
        /* 4 字节（emoji 等）本字库不支持，按单字节跳过，保证不死循环 */
        cp = s[0];
        *p += 1;
    }
    return cp;
}

/* 读 UTF-8 文本文件，按行切段。返回行指针数组（其首元素所在缓冲区与之同寿命）。 */
static char **read_file_lines(const char *path, int *out_n)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "无法打开 %s\n", path);
        return NULL;
    }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz < 0) {
        fclose(f);
        return NULL;
    }
    char *buf = malloc((size_t)sz + 2);
    char **lines = malloc(sizeof(char *) * 256);
    if (!buf || !lines) {
        free(buf);
        free(lines);
        fclose(f);
        return NULL;
    }
    size_t n = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    buf[n] = '\0';

    int cnt = 0;
    char *p = buf;
    while (*p && cnt < 256) {
        char *nl = strpbrk(p, "\r\n");
        if (nl) {
            *nl = '\0';
        }
        if (*p) {
            lines[cnt++] = p;
        }
        if (!nl) {
            break;
        }
        p = nl + 1;
        while (*p == '\r' || *p == '\n') {
            p++;
        }
    }
    *out_n = cnt;
    return lines;
}

int main(int argc, char **argv)
{
    /* 内置示例只用数据表里真实存在的游戏术语（宝可梦名/招式名/属性名）
     * 与 src/ 里的日志原文。手写文案容易混进字库外的字
     * （曾用「效果绝佳」——「佳」不在数据里，导致「缺字」报告失真）。 */
    static const char *defaults[] = {
        "妙蛙种子 喷火龙 皮卡丘 超梦",
        "巨钳螳螂 暴鲤龙 卡比兽 快龙",
        "喷射火焰 十万伏特 地震 剑舞",
        "一般 格斗 飞行 毒 地面 岩石 虫 幽灵 钢 火 水 草",
        "战斗开始！喷火龙 对 巨金怪",
        "喷火龙 使用了 喷射火焰！",
        "会心一击！造成了 187 点伤害",
        "巨金怪 因麻痹无法动弹！",
        "对 皮卡丘 没有效果……",
        "天气恢复正常了",
        "吃剩的东西 回复了 16 点 HP！",
        "第 12 回合",
        "HP 187/240  Lv50  攻击 168 防御 145",
        "选择招式 换人 背包 逃跑 返回",
    };
    const int n_default = (int)(sizeof(defaults) / sizeof(defaults[0]));

    const char **lines = defaults;
    int n_lines = n_default;

    for (int i = 1; i + 1 < argc; i++) {
        if (strcmp(argv[i], "--file") == 0) {
            int n = 0;
            char **raw = read_file_lines(argv[i + 1], &n);
            if (!raw) {
                return 1;
            }
            lines = (const char **)raw;
            n_lines = n;
            break;
        }
    }

    /* 量最长行（中文与 ASCII 都占一格） */
    int max_cols = 1;
    for (int i = 0; i < n_lines; i++) {
        int cols = 0;
        const char *p = lines[i];
        while (*p) {
            next_cp(&p);
            cols++;
        }
        if (cols > max_cols) {
            max_cols = cols;
        }
    }

    int w = PAD_X * 2 + max_cols * FONT_CELL_W;
    int h = PAD_Y * 2 + n_lines * (FONT_CELL_H + LINE_GAP) - LINE_GAP;
    unsigned char *px = calloc((size_t)w * h, 1);
    if (!px) {
        fprintf(stderr, "内存不足\n");
        return 1;
    }

    int missing = 0;
    for (int li = 0; li < n_lines; li++) {
        const char *p = lines[li];
        int cx = PAD_X;
        int cy = PAD_Y + li * (FONT_CELL_H + LINE_GAP);
        while (*p) {
            uint32_t cp = next_cp(&p);
            uint16_t gi = font_lookup(cp);
            if (gi == 0xFFFF || gi >= FONT_GLYPH_COUNT) {
                missing++; /* 字库没有这个字，留空以示缺失 */
                cx += FONT_CELL_W;
                continue;
            }
            const uint8_t *g =
                kFontBitmap + (size_t)gi * FONT_CELL_H * FONT_STRIDE;
            for (int y = 0; y < FONT_CELL_H; y++) {
                for (int x = 0; x < FONT_CELL_W; x++) {
                    int bit = (g[y * FONT_STRIDE + x / 8] >> (7 - (x % 8))) & 1;
                    if (!bit) {
                        continue;
                    }
                    int dx = cx + x;
                    int dy = cy + y;
                    if (dx < 0 || dy < 0 || dx >= w || dy >= h) {
                        continue;
                    }
                    px[(size_t)dy * w + dx] = 255;
                }
            }
            cx += FONT_CELL_W;
        }
    }

    const char *path = "data/font_preview/font_dump.pgm";
    FILE *f = fopen(path, "wb");
    if (!f) {
        fprintf(stderr, "无法写入 %s（先 mkdir data/font_preview）\n", path);
        free(px);
        return 1;
    }
    fprintf(f, "P5\n%d %d\n255\n", w, h);
    fwrite(px, 1, (size_t)w * h, f);
    fclose(f);
    free(px);

    printf("字库：%d 字，格 %dx%d，每字 %d 字节\n", FONT_MAP_COUNT, FONT_CELL_W,
           FONT_CELL_H, FONT_CELL_H * FONT_STRIDE);
    printf("已导出 %s (%dx%d)\n", path, w, h);
    printf("缺字 %d 个%s\n", missing,
           missing ? "（该字符不在字库里，图中会表现为空白）" : "");
    return 0;
}
