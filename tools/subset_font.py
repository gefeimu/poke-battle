"""
subset_font.py - 把思源黑体裁成项目用到的字符子集

为什么需要这一步
----------------
`lv_font_conv` 1.5.3（当前最新）内置的 opentype.js 读不了完整的思源黑体 OTF：
`SourceHanSansSC-Normal.otf` 是 CFF/PostScript 轮廓，含约 6.5 万个字形，
opentype.js 会报 `Offset is outside the bounds of the DataView`。

用 fontTools 先把字表裁到我们实际用的 1700 来个字，CFF 表随之缩小两个数量级，
`lv_font_conv` 就能正常读取。顺带也让后续转换从分钟级降到秒级。

这一步是**幂等的**：同一份字符清单 + 同一个源字体 → 同一份子集字体。

用法：
    python tools/subset_font.py \
        --src data/fonts/SourceHanSansSC-Normal.otf \
        --chars gen/font_chars.txt \
        --out data/fonts/SourceHanSansSC-subset.otf
"""

import argparse
import sys
from pathlib import Path

from fontTools import subset
from fontTools.ttLib import TTFont

ROOT = Path(__file__).resolve().parent.parent

# 半角可见字符全量纳入：官方内置字体也是用 `-r 0x20-0x7f` 整段带的，
# 而 gen/font_chars.txt 里只收集到项目实际用到的那些，不够全。
ASCII = tuple(range(0x20, 0x7F + 1))


def read_codepoints(chars_path):
    """读字符清单，返回码点集合（去重、去空白）。"""
    text = Path(chars_path).read_text(encoding="utf-8")
    cps = {ord(c) for c in text if not c.isspace()}
    cps.update(ASCII)
    return cps


def subset_font(src, out, codepoints):
    """裁字体到指定码点。返回 (原字形数, 新字形数, 原大小, 新大小)。"""
    src_size = Path(src).stat().st_size

    opts = subset.Options()
    # 保留字距调整；LVGL 的 --force-fast-kern-format 需要 kern/GPOS 数据
    opts.layout_features = ["kern", "liga", "locl", "vert"]
    opts.drop_tables += ["DSIG"]
    opts.notdef_outline = True
    # 不重命名字形，保证与源字体一致（便于排查）
    opts.glyph_names = False
    opts.recalc_bounds = True

    font = subset.load_font(str(src), opts)
    n_before = len(font.getGlyphOrder())

    subsetter = subset.Subsetter(options=opts)
    subsetter.populate(unicodes=codepoints)
    subsetter.subset(font)

    n_after = len(font.getGlyphOrder())
    Path(out).parent.mkdir(parents=True, exist_ok=True)
    subset.save_font(font, str(out), opts)
    font.close()

    return n_before, n_after, src_size, Path(out).stat().st_size


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--src", required=True, help="源字体（TTF/OTF）")
    ap.add_argument("--chars", required=True, help="字符清单文件（UTF-8，每字符一个）")
    ap.add_argument("--out", required=True, help="输出子集字体路径")
    args = ap.parse_args()

    if not Path(args.src).exists():
        sys.exit(f"源字体不存在：{args.src}")

    cps = read_codepoints(args.chars)
    print(f"字符清单：{len(cps)} 个码点（含 0x20-0x7F 全段 ASCII）")

    before, after, s_sz, o_sz = subset_font(args.src, args.out, cps)

    print(f"\n源字体   {Path(args.src).name}")
    print(f"  字形 {before:,}  ->  {after:,}")
    print(f"  体积 {s_sz/1048576:.1f} MB  ->  {o_sz/1024:.1f} KB"
          f"  （缩到 {o_sz/s_sz*100:.1f}%）")
    print(f"\n输出 {args.out}")

    # 自检：子集字体里这些码点是否都在
    f = TTFont(args.out)
    cmap = f.getBestCmap()
    missing = [cp for cp in cps if cp not in cmap]
    f.close()
    if missing:
        sample = "".join(chr(c) for c in missing[:40])
        sys.exit(f"\n!! 子集后有 {len(missing)} 个码点缺失：{sample}")
    print("自检：全部码点均存在于子集字体的 cmap 中")


if __name__ == "__main__":
    main()
