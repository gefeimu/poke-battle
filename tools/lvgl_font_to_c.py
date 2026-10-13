"""
lvgl_font_to_c.py - 把 lv_font_conv 生成的 LVGL 字体转成项目自有格式

为什么要转
----------
lv_font_conv 输出的 .c 有两个问题，导致 src/ 用不了：

1. 所有数组都是 `static` 的，外部拿不到；
2. 依赖 lvgl.h（LV_ATTRIBUTE_LARGE_CONST / lv_font_t / cmap 结构体 …）。

而 src/ 的约定是**零依赖纯 C、能在主机侧完整测试**，不能为了字体把整个
LVGL 拉进来。所以这里做一次转换：字形数据一字不改，只把

    cmaps[] + unicode_list_N[] + glyph_id_ofs_list_N[]

那套「区间 + 稀疏列表」的紧凑表示**摊平**成一张按码点升序的查找表。
代价是每个码点多占 6 字节（16px 那份 1283 条约 10 KB），换来的是
渲染端查找只有十几行、不用理解四种 cmap 格式。8 MB Flash 里可以忽略。

转换过程带三项自检（都踩过或差点踩到）
--------------------------------------
* **位图总长 == 各字形尺寸之和**。曾经 hex 正则只匹配两位，把所有
  0x00-0x0F 的字节都丢了（lv_font_conv 对小于 0x10 的值只写一位十六进制，
  形如 `0x0` / `0x9` / `0xf`），位图短了三分之一却不报错。
* **码点表升序且无重复**，否则渲染端的二分查找会静默查不到。
* **字形索引都在范围内**，否则渲染时会读越界。

用法
----
    python tools/lvgl_font_to_c.py --out gen/poke_font.c \\
        gen/font_app_16.c gen/font_app_12.c gen/font_hp_10.c
"""

import argparse
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent

# cmap 类型后缀决定「怎么算数量」，前缀 SPARSE 决定「码点从哪来」。
# 注意：TINY / FULL 只表示**偏移表元素的宽度**（uint8 / uint16），
# 跟稀不稀疏无关 —— 按 "TINY" 判连续会把稀疏表当连续算，字形索引直接飞掉。
_TYPE_FULL_RE = re.compile(r"FULL$")


def cint(text):
    """
    C 字面量转 int。

    不能用 int(x, 0)：那份输出里存在 `05` 这种带前导零却没有 0x 前缀的值，
    int('05', 0) 会抛 ValueError。
    """
    t = text.strip()
    if t.lower().startswith("0x"):
        return int(t, 16)
    return int(t, 10)


def read_array(text, name):
    """取 `name[] = { ... };` 里的全部整数。"""
    m = re.search(rf"\b{name}\s*\[\s*\]\s*=\s*\{{(.*?)\n\}};", text, re.S)
    if not m:
        return None
    body = m.group(1)
    return [cint(x) for x in re.findall(r"0x[0-9A-Fa-f]+|\d+", body)]


def read_bitmap(text, name="glyph_bitmap"):
    """
    取位图字节。

    **正则必须是 {1,2} 而不是 {2}** —— lv_font_conv 对小于 0x10 的字节
    只写一位十六进制（`0x0` / `0x9` / `0xf`），只匹配两位会把 0x00-0x0F
    整批丢掉，位图悄悄变短，画出来是一堆噪点而不是报错。
    """
    m = re.search(rf"\b{name}\s*\[\s*\]\s*=\s*\{{(.*?)\n\}};", text, re.S)
    if not m:
        raise SystemExit(f"找不到 {name}")
    return bytes(int(x, 16) for x in re.findall(r"0x([0-9A-Fa-f]{1,2})", m.group(1)))


def read_glyph_dsc(text):
    """取字形描述数组。索引 0 是 LVGL 的保留槽。"""
    m = re.search(r"\bglyph_dsc\s*\[\s*\]\s*=\s*\{(.*?)\n\};", text, re.S)
    if not m:
        raise SystemExit("找不到 glyph_dsc")
    body = m.group(1)
    out = []
    for mm in re.finditer(
            r"\{\s*\.bitmap_index\s*=\s*(-?\d+)\s*,\s*\.adv_w\s*=\s*(-?\d+)\s*,"
            r"\s*\.box_w\s*=\s*(-?\d+)\s*,\s*\.box_h\s*=\s*(-?\d+)\s*,"
            r"\s*\.ofs_x\s*=\s*(-?\d+)\s*,\s*\.ofs_y\s*=\s*(-?\d+)\s*\}", body):
        out.append(dict(
            bitmap_index=int(mm.group(1)), adv_w=int(mm.group(2)),
            box_w=int(mm.group(3)), box_h=int(mm.group(4)),
            ofs_x=int(mm.group(5)), ofs_y=int(mm.group(6)),
        ))
    if not out:
        raise SystemExit("glyph_dsc 解析出 0 条")
    return out


def read_cmaps(text):
    """取 cmap 段。每段描述一块码点怎么映射到字形索引。"""
    m = re.search(r"\bcmaps\s*\[\s*\]\s*=\s*\{(.*?)\n\};", text, re.S)
    if not m:
        raise SystemExit("找不到 cmaps")
    out = []
    for mm in re.finditer(
            r"\.range_start\s*=\s*(\d+)\s*,\s*\.range_length\s*=\s*(\d+)\s*,"
            r"\s*\.glyph_id_start\s*=\s*(\d+)\s*,\s*\.unicode_list\s*=\s*(\w+)\s*,"
            r"\s*\.glyph_id_ofs_list\s*=\s*(\w+)\s*,\s*\.list_length\s*=\s*(\d+)\s*,"
            r"\s*\.type\s*=\s*(\w+)", m.group(1)):
        out.append(dict(
            range_start=int(mm.group(1)), range_length=int(mm.group(2)),
            glyph_id_start=int(mm.group(3)), unicode_list=mm.group(4),
            glyph_id_ofs_list=mm.group(5), list_length=int(mm.group(6)),
            type=mm.group(7),
        ))
    if not out:
        raise SystemExit("cmaps 解析出 0 段")
    return out


def read_metrics(text):
    m = re.search(r"\.line_height\s*=\s*(\d+)", text)
    lh = int(m.group(1)) if m else 0
    m = re.search(r"\.base_line\s*=\s*(-?\d+)", text)
    bl = int(m.group(1)) if m else 0
    m = re.search(r"\.bpp\s*=\s*(\d+)", text)
    bpp = int(m.group(1)) if m else 4
    m = re.search(r"^\s*(?:static\s+)?(?:const\s+)?lv_font_t\s+(\w+)\s*=",
                  text, re.M)
    name = m.group(1) if m else "font"
    return lh, bl, bpp, name


def cmap_stride(bpp):
    """一个 box_w 宽的扫描行占多少字节（连续位打包，再乘 box_h 得字形大小）"""
    return (bpp + 7) // 8


def glyph_bytes(g, bpp):
    """
    一个字形占多少字节。

    **LVGL 的 4bpp 位图是连续位打包，行间不补齐字节**：
    占 ceil(box_w * box_h * bpp / 8)，不是 ceil(box_w*bpp/8) * box_h。
    按后者算会全错（实测 40 个连续字形全部符合连续打包）。
    """
    return -(-g["box_w"] * g["box_h"] * bpp // 8)


def flatten_map(cmaps, lists, ofs_lists):
    """把 cmap 段摊平成 (码点, 字形索引) 列表。"""
    pairs = []
    for cm in cmaps:
        t = cm["type"]
        sparse = "SPARSE" in t
        full = bool(_TYPE_FULL_RE.search(t))

        if sparse:
            ul = lists.get(cm["unicode_list"])
            if ul is None:
                raise SystemExit(f"cmap 引用了不存在的 {cm['unicode_list']}")
            if full:
                # SPARSE_FULL：数量仍由 range_length 给（= max offset + 1），
                # 但只有 unicode_list 里列出的偏移才有字形
                n = len(ul)
            else:
                n = len(ul)
        else:
            n = cm["range_length"] if not full else cm["list_length"]
            ul = None

        gofs = ofs_lists.get(cm["glyph_id_ofs_list"]) if full else None

        for i in range(n):
            if sparse:
                cp = cm["range_start"] + ul[i]
            else:
                cp = cm["range_start"] + i
            gid = cm["glyph_id_start"] + i
            if gofs is not None and i < len(gofs):
                gid += gofs[i]
            pairs.append((cp, gid))
    return pairs


def convert(path):
    text = Path(path).read_text(encoding="utf-8", errors="replace")
    lh, bl, bpp, name = read_metrics(text)
    bitmap = read_bitmap(text)
    glyphs = read_glyph_dsc(text)
    cmaps = read_cmaps(text)

    lists = {}
    for mm in re.finditer(r"\b(unicode_list_\d+)\s*\[\s*\]\s*=\s*\{(.*?)\n\};",
                          text, re.S):
        lists[mm.group(1)] = [cint(x) for x in
                              re.findall(r"0x[0-9A-Fa-f]+|\d+", mm.group(2))]
    ofs_lists = {}
    for mm in re.finditer(r"\b(glyph_id_ofs_list_\d+)\s*\[\s*\]\s*=\s*\{(.*?)\n\};",
                          text, re.S):
        ofs_lists[mm.group(1)] = [cint(x) for x in
                                  re.findall(r"0x[0-9A-Fa-f]+|\d+", mm.group(2))]

    pairs = flatten_map(cmaps, lists, ofs_lists)
    pairs.sort()

    # ---- 自检 1：位图总长 == 各字形尺寸之和 ----
    want = sum(glyph_bytes(g, bpp) for g in glyphs)
    # 最后一个字形之后可能还有填充，所以允许 >= 但不允许 <
    if len(bitmap) < want:
        raise SystemExit(
            f"{Path(path).name}: 位图太短 —— 需要 {want:,} 字节，"
            f"实际 {len(bitmap):,}（八成是 hex 正则漏了单字符的值）")
    # 顺便检查最大偏移没越界
    for i, g in enumerate(glyphs):
        if g["bitmap_index"] + glyph_bytes(g, bpp) > len(bitmap):
            raise SystemExit(
                f"{Path(path).name}: 字形 {i} 的位图越界 "
                f"({g['bitmap_index']} + {glyph_bytes(g, bpp)} > {len(bitmap)})")

    # ---- 自检 2：码点升序、无重复 ----
    for i in range(1, len(pairs)):
        if pairs[i][0] == pairs[i - 1][0]:
            raise SystemExit(
                f"{Path(path).name}: 码点 U+{pairs[i][0]:04X} 重复"
                f"（二分查找会静默取到其中一个）")
    # ---- 自检 3：字形索引在范围内 ----
    for cp, gid in pairs:
        if gid >= len(glyphs):
            raise SystemExit(
                f"{Path(path).name}: U+{cp:04X} 指向字形 {gid}，"
                f"但只有 {len(glyphs)} 个字形")

    print(f"  {Path(path).name}: {name}")
    print(f"    字号参数 line_height={lh} base_line={bl} bpp={bpp}")
    print(f"    字形 {len(glyphs)}（含保留槽 0），映射 {len(pairs)} 条，"
          f"位图 {len(bitmap):,} 字节")
    print(f"    位图自检：需要 {want:,} 字节，实际 {len(bitmap):,}（含尾部填充）")

    return dict(name=name, line_height=lh, base_line=bl, bpp=bpp,
                bitmap=bitmap, glyphs=glyphs, pairs=pairs, path=str(path))


def c_ident(lvgl_name):
    """font_app_16 -> kFontApp16"""
    parts = [p for p in lvgl_name.split("_") if p]
    if parts and parts[0].lower() == "font":
        parts = parts[1:]
    tail = "".join(p.capitalize() for p in parts)
    return "kFont" + tail


def emit(fonts, out_c, out_h):  # noqa: C901
    src = []
    src.append("/*")
    src.append(" * 由 tools/lvgl_font_to_c.py 自动生成 —— 不要手工编辑。")
    src.append(" *")
    src.append(" * 源：lv_font_conv 输出的 LVGL 字体（gen/font_app_*.c）。")
    src.append(" * 转换只做「重新排列 + 展平码点查找表」，字形数据一字不改。")
    src.append(" * 详细理由见 src/poke_font.h 的注释。")
    src.append(" */")
    src.append('#include "poke_font.h"')
    src.append("")

    hdr = []
    stem = Path(out_h).stem.upper()
    hdr.append("/* 由 tools/lvgl_font_to_c.py 自动生成 —— 不要手工编辑 */")
    hdr.append("/*")
    hdr.append(" * 这里只有**数据**（位图、字形描述、码点表），查找与绘制在")
    hdr.append(" * src/poke_font.c 与 src/poke_render.c 里。")
    hdr.append(" *")
    hdr.append(" * 文件名特意不叫 poke_font.h：那会与 src/poke_font.h 重名，")
    hdr.append(" * 而 #include \"poke_font.h\" 在同目录优先，结果是生成的头文件")
    hdr.append(" * 包含自己，编译报 unknown type name（踩过一次）。")
    hdr.append(" */")
    hdr.append(f"#ifndef GEN_{stem}_H")
    hdr.append(f"#define GEN_{stem}_H")
    hdr.append("")
    hdr.append('#include "poke_font.h"')
    hdr.append("")

    for f in fonts:
        ident = c_ident(f["name"])
        tag = ident[5:] or "X"

        src.append(f"/* ============================ {f['name']} ============================ */")
        src.append(f"/* {f['bpp']}bpp，{len(f['glyphs'])} 字形，"
                   f"{len(f['pairs'])} 条映射，位图 {len(f['bitmap']):,} 字节 */")
        src.append("")

        # 位图
        src.append(f"static const uint8_t kBitmap{tag}[] = {{")
        bm = f["bitmap"]
        for i in range(0, len(bm), 16):
            row = " ".join(f"0x{b:02X}," for b in bm[i:i + 16])
            src.append("    " + row)
        src.append("};")
        src.append("")

        # 字形描述
        src.append(f"static const GlyphDsc kGlyphs{tag}[] = {{")
        for g in f["glyphs"]:
            src.append(f"    {{ {g['bitmap_index']}, {g['adv_w']}, "
                       f"{g['box_w']}, {g['box_h']}, "
                       f"{g['ofs_x']}, {g['ofs_y']} }},")
        src.append("};")
        src.append("")

        # 码点映射
        src.append(f"static const FontMapEntry kMap{tag}[] = {{")
        pairs = f["pairs"]
        for i in range(0, len(pairs), 4):
            row = " ".join(f"{{ {cp}, {gid} }}," for cp, gid in pairs[i:i + 4])
            src.append("    " + row)
        src.append("};")
        src.append("")

        src.append(f"const BitmapFont {ident} = {{")
        src.append(f'    "{f["name"]}", {f["bpp"]}, {f["line_height"]}, '
                   f'{f["base_line"]},')
        src.append(f"    {len(f['glyphs'])}, {len(pairs)},")
        src.append(f"    kBitmap{tag}, kGlyphs{tag}, kMap{tag},")
        src.append("};")
        src.append("")

        hdr.append(f"/* {f['name']}：{f['bpp']}bpp，line_height={f['line_height']}，"
                   f"{len(f['pairs'])} 条映射 */")
        hdr.append(f"extern const BitmapFont {ident};")

    hdr.append("")
    hdr.append(f"#endif /* GEN_{stem}_H */")
    hdr.append("")

    Path(out_c).write_text("\n".join(src), encoding="utf-8")
    Path(out_h).write_text("\n".join(hdr), encoding="utf-8")

    total_bitmap = sum(len(f["bitmap"]) for f in fonts)
    print(f"\n输出 {out_c}")
    print(f"     {out_h}")
    print(f"     位图合计 {total_bitmap:,} 字节 "
          f"({total_bitmap / 1024:.1f} KB)，源文本 "
          f"{Path(out_c).stat().st_size / 1024:.0f} KB")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("fonts", nargs="+", help="lv_font_conv 生成的 .c 文件")
    ap.add_argument("--guard", default=None,
                    help="输出头文件的 include guard（默认按文件名推）")
    ap.add_argument("--out", required=True,
                    help="输出 .c 路径。配套的 .h 会取同目录的 "
                         "<stem>_data.h —— **不能与 .c 同名**，否则 "
                         "include \"poke_font.h\" 会解析到生成头文件自己，"
                         "形成循环包含（踩过一次）")
    ap.add_argument("--header", help="输出 .h 路径（默认 <stem>_data.h）")
    args = ap.parse_args()

    out_c = Path(args.out)
    if args.header:
        out_h = Path(args.header)
    elif out_c.stem.endswith("_data"):
        out_h = out_c.with_suffix(".h")        # poke_font_data.c -> .h
    else:
        out_h = out_c.with_name(out_c.stem + "_data.h")

    print("转换 LVGL 字体 -> 项目自有格式：")
    fonts = [convert(p) for p in args.fonts]
    # 字号大的排前面，方便对照
    fonts.sort(key=lambda f: -f["line_height"])
    emit(fonts, out_c, out_h)
    return 0


if __name__ == "__main__":
    sys.exit(main())
