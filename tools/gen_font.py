"""
扫描项目数据表里实际用到的汉字，生成子集点阵字库。

为什么要子集化：
完整 GB2312 点阵是 6763 字 * 32 字节 = 216 KB，在 8MB Flash 里占比过高。
宝可梦对战实际只用到 800~1400 个字，子集化后约 40 KB。

需要字体文件：任意中文 TTF/OTF（如思源黑体）。
用法：
    python tools/gen_font.py --font C:/path/to/SourceHanSansCN-Regular.otf
"""

import argparse
import re
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont

ROOT = Path(__file__).resolve().parent.parent
GEN = ROOT / "gen"
GEN.mkdir(parents=True, exist_ok=True)

# 14x14 而不是 12x12：12px 的中文笔画粘连严重（「战斗精灵」已难辨认，
# 见 data/font_preview/size_compare.png）。14x14 每字 28 字节，与
# 12x12 逐像素比较只多 4 字节/字，1630 字共多 6.4 KB，换来可读性。
CELL_W, CELL_H = 14, 14

# 每行的字节数。**不要用 CELL_W // 8**：14 不是 8 的倍数，整数除法得 1，
# 会只写前 8 个像素、丢掉每行后 6 个像素，而消费端按 ceil 出的 2 字节
# 索引，两边错位导致字形全是散点（「一」本该是一横却出现 47 个点）。
FONT_STRIDE = (CELL_W + 7) // 8

# UI 固定文案，必须包含
UI_STRINGS = [
    "战斗背包精灵逃跑选择招式使用当前等级经验生命普通异常中毒麻痹灼伤冰冻睡眠",
    "对方我方赢了输了平局倒下不能再战继续重新开始设置返回确定取消",
    "属性特殊防御速度攻击单只全选换人恢复训练对战练习",
    "妙蛙种子妙蛙草妙蛙花皮卡丘杰尼龟喷火龙超梦梦幻妖",
    "提示左右上下移动长按快速返回错误无效溢出冻结",
]

# 常用标点与数字
EXTRA_CHARS = "0123456789:/%+-.<>!?,()[]ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz"


def collect_chars():
    """
    汇总字库需要的全部字符。

    来源分三类，**都按「固件里真的会出现」收集，不做超集**：

      1. **生成的 C 数据表**（`gen/*.c`）——六张表的中文名以 `name_cn`
         字符串字面量存在结构体初始化里，扫字面量即可精确拿到。
         早期版本改成按 Veekun CSV 全量收集，结果把**不在我们表里**的
         八代道具名也带进来（例如「糬」来自一个 383 项之外的道具），
         白白撑大字库还引入无法覆盖的生僻字。
      2. **引擎源码**（`src/*.c`）——战斗日志文案在这里，只扫 gen/ 会漏
         26 个字（濒/稳/第/被/么/什…），UI 显示日志时那些字是空白。
      3. **手写常量**（`UI_STRINGS` / `EXTRA_CHARS`）——不属于任何表的
         界面词与半角字符。
    """
    chars = set()

    # ---- 来源3：手写常量 ----
    for s in UI_STRINGS:
        chars.update(s.strip())
    chars.update(EXTRA_CHARS)

    # ---- 来源1+2：扫源码字面量（gen/ 与 src/ 都要） ----
    src_dirs = [GEN, ROOT / "src"]
    for d in src_dirs:
        if not d.exists():
            continue
        for path in sorted(d.glob("*.c")):
            text = path.read_text(encoding="utf-8")
            for m in re.finditer(r'"([^"\\]*)"', text):
                chars.update(m.group(1))

    # 只保留可打印字符
    printable = {c for c in chars if c.isprintable() and c not in "\r\n\t"}
    # 按 Unicode 排序，保证输出稳定
    return sorted(printable, key=ord)


def render_font(font_path, chars, font_size=CELL_H):
    """用 PIL 渲染每个字为位图，返回 {char: CELL_WxCELL_H 二值矩阵}。"""
    font = ImageFont.truetype(font_path, font_size)
    glyphs = {}

    for ch in chars:
        img = Image.new("L", (CELL_W, CELL_H), 0)
        draw = ImageDraw.Draw(img)
        # anchor="mm" 即画布几何中心。曾试过「先画 2 倍图画布量 bbox
        # 再平移」来补偿中文字体重心偏下，结果把字画到了画布外——
        # bbox 恰好居中时偏移算出来是 0，再减半画布就成了负坐标，
        # 整张字库全空。实测等线 14px 配 anchor=mm 本身就对，
        # 保持最简写法。
        try:
            draw.text((CELL_W // 2, CELL_H // 2), ch, fill=255, font=font,
                      anchor="mm")
        except TypeError:
            draw.text((0, 0), ch, fill=255, font=font)

        glyphs[ch] = [[1 if img.getpixel((x, y)) > 128 else 0 for x in range(CELL_W)]
                      for y in range(CELL_H)]

    return glyphs


def pack_glyph(bits):
    """
    1bpp 打包：每行 ceil(CELL_W/8) 字节，行优先，高位在前。

    注意 CELL_W=14 不是 8 的整数倍，所以每行第 2 字节只用低 6 位，
    高 2 位是填充 0。解码端必须只取前 CELL_W 位，多余位恒为 0
    —— 这样渲染时不会画出多余的点。
    """
    out = bytearray()
    for y in range(CELL_H):
        for byte_idx in range(FONT_STRIDE):
            b = 0
            for bit in range(8):
                px = byte_idx * 8 + bit
                # 行尾不足 8 像素时补 0（CELL_W=14 时每行末 2 位是填充）
                b = (b << 1) | (bits[y][px] if px < CELL_W else 0)
            out.append(b)
    return bytes(out)


def emit(chars, glyphs, out_prefix="font_cn"):
    # 每 8 个字对齐到一个 uint32_t 地址，编码时可直接 shift 取位
    align = 8

    # 建立 char -> 索引
    index = {ch: i for i, ch in enumerate(chars)}
    padded = list(chars)
    while len(padded) % align != 0:
        padded.append("\0")

    # .h
    lines = [
        "/* 由 tools/gen_font.py 自动生成 */",
        "#ifndef FONT_CN_H",
        "#define FONT_CN_H",
        "",
        "#include <stdint.h>",
        "",
        f"#define FONT_CELL_W {CELL_W}",
        f"#define FONT_CELL_H {CELL_H}",
        f"#define FONT_STRIDE {FONT_STRIDE}   /* 每行字节数 = ceil(CELL_W/8)，"
        f"行尾多余位为 0 */",
        f"#define FONT_GLYPH_COUNT {len(padded)}",
        f"#define FONT_ALIGN {align}",
        "",
        f"/* 1bpp 位图，每字 {CELL_H * FONT_STRIDE} 字节（{FONT_STRIDE} 字节/行），行优先、高位在前 */",
        f"extern const uint8_t kFontBitmap[{len(padded) * CELL_H * FONT_STRIDE}];",
        "",
        "/* UTF-8 字符串首字节 -> 字形索引映射表，覆盖常用汉字 */",
        "typedef struct {",
        "    uint16_t codepoint;",
        "    uint16_t glyph_index;",
        "} FontMapEntry;",
        "",
        "extern const FontMapEntry kFontMap[];",
        f"#define FONT_MAP_COUNT {len(chars)}",
        "",
        "/* 按码点查字形索引，查不到返回 0xFFFF */",
        "uint16_t font_lookup(uint32_t codepoint);",
        "",
        "/* 绘制一段 UTF-8 文本到 RGB565 缓冲区 */",
        "void font_draw_text(uint16_t *buf, int buf_w, int buf_h, int x, int y,",
        "                   uint16_t color, const char *utf8);",
        "",
        "#endif /* FONT_CN_H */",
    ]
    (GEN / f"{out_prefix}.h").write_text("\n".join(lines), encoding="utf-8")

    # .c
    lines = [
        "/* 由 tools/gen_font.py 自动生成 */",
        f'#include "{out_prefix}.h"',
        "",
        "const uint8_t kFontBitmap[] = {",
    ]
    data = bytearray()
    for ch in padded:
        if ch == "\0":
            data.extend(pack_glyph([[0] * CELL_W for _ in range(CELL_H)]))
        else:
            data.extend(pack_glyph(glyphs[ch]))
    for i in range(0, len(data), 16):
        lines.append("    " + " ".join(f"0x{b:02X}," for b in data[i : i + 16]))
    lines += ["};", ""]

    lines += [
        "const FontMapEntry kFontMap[] = {",
    ]
    for ch in chars:
        lines.append(f"    {{ 0x{ord(ch):04X}, {index[ch]} }},")
    lines += ["};", ""]

    # 字模打包：按 align 对齐后每个 uint32 存一个字
    lines += [
        "uint16_t font_lookup(uint32_t codepoint)",
        "{",
        "    uint16_t lo = 0, hi = FONT_MAP_COUNT;",
        "    while (lo < hi) {",
        "        uint16_t mid = (uint16_t)(lo + (hi - lo) / 2);",
        "        if (kFontMap[mid].codepoint == codepoint) return kFontMap[mid].glyph_index;",
        "        if (kFontMap[mid].codepoint < codepoint) lo = (uint16_t)(mid + 1);",
        "        else hi = mid;",
        "    }",
        "    return 0xFFFF;",
        "}",
        "",
        "/* 解码一个 UTF-8 字符，返回码点并推进指针 */",
        "static uint32_t utf8_next(const char **p)",
        "{",
        "    const uint8_t *s = (const uint8_t *)*p;",
        "    uint32_t cp;",
        "    if (s[0] < 0x80) { cp = s[0]; *p += 1; }",
        "    else if ((s[0] & 0xE0) == 0xC0) { cp = (uint32_t)((s[0] & 0x1F) << 6) | (s[1] & 0x3F); *p += 2; }",
        "    else if ((s[0] & 0xF0) == 0xE0) { cp = (uint32_t)((s[0] & 0x0F) << 12) | ((uint32_t)(s[1] & 0x3F) << 6) | (s[2] & 0x3F); *p += 3; }",
        "    else { cp = s[0]; *p += 1; }",
        "    return cp;",
        "}",
        "",
        "void font_draw_text(uint16_t *buf, int buf_w, int buf_h, int x, int y,",
        "                   uint16_t color, const char *utf8)",
        "{",
        "    const char *p = utf8;",
        "    int cx = x;",
        "    while (*p) {",
        "        uint32_t cp = utf8_next(&p);",
        "        if (cp < 0x20) continue;",
        "        uint16_t gi = font_lookup(cp);",
        "        if (gi == 0xFFFF) { cx += FONT_CELL_W / 2; continue; }",
        "        const uint8_t *g = kFontBitmap + (size_t)gi * FONT_CELL_H * FONT_STRIDE;",
        "        for (int gy = 0; gy < FONT_CELL_H; gy++) {",
        "            for (int gx = 0; gx < FONT_CELL_W; gx++) {",
        "                int bit = g[gy * FONT_STRIDE + gx / 8] >> (7 - (gx % 8)) & 1;",
        "                if (!bit) continue;",
        "                int px = cx + gx, py = y + gy;",
        "                if (px < 0 || py < 0 || px >= buf_w || py >= buf_h) continue;",
        "                buf[py * buf_w + px] = color;",
        "            }",
        "        }",
        "        cx += FONT_CELL_W;",
        "    }",
        "}",
    ]

    # 修正 kFontBitmap 声明长度（对齐后）
    src = "\n".join(lines)
    src = src.replace(
        f"const uint8_t kFontBitmap[] = {{",
        f"const uint8_t kFontBitmap[{len(padded) * CELL_H * FONT_STRIDE}] = {{",
    )
    (GEN / f"{out_prefix}.c").write_text(src, encoding="utf-8")

    # 码点表（给固件做 UTF-8 匹配用）
    table = GEN / "font_chars.txt"
    table.write_text("".join(chars), encoding="utf-8")

    return len(chars), len(data), table


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--font", required=True, help="中文字体文件路径 (TTF/OTF)")
    # 默认跟随字格高度：字号必须 >= CELL_H，否则字的上下笔画会被
    # 画布裁掉（曾出现字号 12 / 字格 14 的组合，实测「妙」只剩 23 个
    # 亮点、正常应有 45 个，字形支离破碎）。
    ap.add_argument("--size", type=int, default=CELL_H,
                    help=f"字号，默认 = 字格高度 {CELL_H}")
    ap.add_argument("--prefix", default="font_cn")
    args = ap.parse_args()

    chars = collect_chars()
    print(f"收集到 {len(chars)} 个不重复字符")

    glyphs = render_font(args.font, chars, font_size=args.size)
    n, size, table = emit(chars, glyphs, args.prefix)

    print(f"字库已生成：{n} 字，{size:,} 字节 ({size / 1024:.1f} KB)")
    print(f"对比：完整 GB2312 216 KB -> 节省 {(1 - size / (6763 * 32)) * 100:.1f}%")
    print(f"  {GEN / (args.prefix + '.h')}")
    print(f"  {GEN / (args.prefix + '.c')}")
    print(f"  {table}")


if __name__ == "__main__":
    main()