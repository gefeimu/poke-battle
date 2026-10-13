"""
gen_lvgl_font.py - 用 lv_font_conv 生成项目用的 LVGL 中文字体

流程
----
  1. （可选）用 fontTools 把源字体裁成项目用字子集
  2. 调 lv_font_conv 生成 LVGL 格式的 .c
  3. 自检：字形数、位图长度、字符覆盖

为什么需要第 1 步
-----------------
`lv_font_conv` 内置的 opentype.js 读不了思源黑体那种「CFF 轮廓 + 6 万字形」
的 OTF（报 `Offset is outside the bounds of the DataView`）。先裁成 1700 来个字，
CFF 表缩小两个数量级后就能读。TTF（TrueType 轮廓）字体通常不需要这步，
但裁一下也能让转换从分钟级降到秒级。

依赖
----
  lv_font_conv    Node 工具，装在隔离目录（见 README）
  fonttools       Python 包，用于子集化（--no-subset 时不使用）

用法
----
  python tools/gen_lvgl_font.py \\
      --font data/fonts/SmileySans-Oblique.ttf \\
      --chars gen/font_chars.txt \\
      --out gen/font_app_16.c \\
      --name font_app_16
"""

import argparse
import os
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent

# 与官方内置字体一致的参数：4bpp 抗锯齿、不压缩、不预滤波。
#
# --no-prefilter 不是可有可无的：LVGL 默认开启逐行预滤波，解码端必须做逆运算。
# 我们的渲染器（tools/render_lvgl_font.py）按原始 4bpp 解，所以必须关掉，
# 否则解出来是像素噪声。官方内置字体同样带这个参数。
FONT_CONV_ARGS = [
    "--bpp", "4",
    "--format", "lvgl",
    "--no-compress",
    "--no-prefilter",
    "--force-fast-kern-format",
    "--range", "0x20-0x7F",     # ASCII 全段，官方内置字体也是这么带的
]

NODE = os.environ.get(
    "NODE_BIN",
    r"C:\Users\baigu\.workbuddy\binaries\node\versions\22.22.2-6\node.exe")
FONT_CONV = os.environ.get(
    "LV_FONT_CONV",
    r"C:\Users\baigu\.workbuddy\binaries\node\workspace"
    r"\node_modules\lv_font_conv\lv_font_conv.js")


def read_chars(path):
    """读字符清单，去重去空白。"""
    text = Path(path).read_text(encoding="utf-8")
    seen = []
    s = set()
    for c in text:
        if c.isspace() or c in s:
            continue
        s.add(c)
        seen.append(c)
    return "".join(seen)


def cint(s):
    """解析 C 字面量整数。

    不能用 int(s, 0)：生成器会把十进制写成带前导零的形式（如 `05`），
    Python 的 base=0 解析会拒绝这种写法（`invalid literal for int() with
    base 0: '05'`）。所以显式区分 0x 前缀与十进制。
    """
    s = s.strip()
    return int(s, 16) if s.lower().startswith(("0x", "-0x")) else int(s, 10)


def subset(src, chars, out):
    """用 fonttools 裁字体。失败时返回 None（由调用方决定是否继续）。"""
    try:
        from fontTools import subset as ft_subset
    except ImportError:
        print("  fonttools 不可用，跳过子集化")
        return None

    cps = {ord(c) for c in chars} | set(range(0x20, 0x80))
    opts = ft_subset.Options()
    opts.layout_features = ["kern", "liga", "locl", "vert"]
    opts.drop_tables += ["DSIG"]
    opts.glyph_names = False

    try:
        font = ft_subset.load_font(str(src), opts)
    except Exception as e:
        print(f"  子集化失败（源字体可能不完整）：{e}")
        return None

    n0 = len(font.getGlyphOrder())
    sub = ft_subset.Subsetter(options=opts)
    sub.populate(unicodes=cps)
    sub.subset(font)
    n1 = len(font.getGlyphOrder())
    ft_subset.save_font(font, str(out), opts)
    font.close()
    print(f"  子集化：{n0:,} 字形 -> {n1:,}，"
          f"{src.stat().st_size/1048576:.1f} MB -> {out.stat().st_size/1024:.0f} KB")
    return out


def run_conv(font, chars, out, name, size):
    if not Path(NODE).exists():
        sys.exit(f"找不到 Node：{NODE}（可用环境变量 NODE_BIN 指定）")
    if not Path(FONT_CONV).exists():
        sys.exit(f"找不到 lv_font_conv：{FONT_CONV}（可用环境变量 LV_FONT_CONV 指定）")

    cmd = [NODE, FONT_CONV, "--font", str(font), "--size", str(size),
           *FONT_CONV_ARGS, "--symbols", chars,
           "-o", str(out), "--lv-font-name", name]
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode != 0:
        sys.exit(f"lv_font_conv 失败：\n{r.stdout}\n{r.stderr}")


def verify(out, chars, bpp, size):
    """自检：字形数、位图长度、字符覆盖。"""
    text = Path(out).read_text(encoding="utf-8", errors="replace")

    m = re.search(r"glyph_bitmap\[\]\s*=\s*\{(.*?)\n\};", text, re.S)
    # hex 位数写 {1,2}：生成器对 <0x10 的字节只写一位（0x0 / 0x9 / 0xf），
    # 写 {2} 会把 0x00-0x0F 整批丢掉，位图长度短掉三分之一。
    bitmap = bytes(int(x, 16) for x in
                   re.findall(r"0x([0-9A-Fa-f]{1,2})", m.group(1)))

    glyphs = []
    mg = re.search(r"glyph_dsc\[\]\s*=\s*\{(.*?)\n\};", text, re.S)
    for ent in re.finditer(r"\{([^{}]*)\}", mg.group(1)):
        f = ent.group(1)
        kv = {}
        for part in f.split(","):
            if "=" in part:
                k, v = part.split("=", 1)
                kv[k.strip().lstrip(".")] = cint(v)
        if kv:
            glyphs.append(kv)

    want = sum(-(-g["box_w"] * g["box_h"] * bpp // 8) for g in glyphs)
    print(f"  字形 {len(glyphs)}  位图 {len(bitmap):,} 字节 "
          f"({len(bitmap)/1024:.1f} KB)")
    if want != len(bitmap):
        sys.exit(f"  !! 位图长度自检失败：字形尺寸之和 {want:,} != 实际 {len(bitmap):,}")

    # 覆盖：清单里每个字符都要能查到字形
    cmaps = []
    for ent in re.finditer(r"\{([^{}]*)\}", re.search(
            r"cmaps\[\]\s*=\s*\{(.*?)\n\};", text, re.S).group(1)):
        f = ent.group(1)
        def g_(k):
            mm = re.search(rf"\.{k}\s*=\s*([^,}}]+)", f)
            if not mm:
                return None
            v = mm.group(1).strip()
            if v == "NULL":
                return None
            if v.startswith(("unicode_list", "glyph_id_ofs_list")):
                return v          # 数组名，原样返回
            if k == "type":
                return v          # 枚举名（LV_FONT_FMT_TXT_CMAP_*），不是数字
            return cint(v)
        cmaps.append(dict(start=g_("range_start"), length=g_("range_length"),
                          gid=g_("glyph_id_start"), ul=g_("unicode_list"),
                          gol=g_("glyph_id_ofs_list"), t=str(g_("type"))))
    lists = {}
    for mm in re.finditer(r"unicode_list(_\d+)?\[\]\s*=\s*\{(.*?)\n\};", text, re.S):
        lists["unicode_list" + (mm.group(1) or "")] = [
            cint(x) for x in re.findall(r"0x[0-9A-Fa-f]+|\d+", mm.group(2))]
    ofs = {}
    for mm in re.finditer(r"glyph_id_ofs_list(_\d+)?\[\]\s*=\s*\{(.*?)\n\};", text, re.S):
        ofs["glyph_id_ofs_list" + (mm.group(1) or "")] = [
            cint(x) for x in re.findall(r"0x[0-9A-Fa-f]+|\d+", mm.group(2))]

    def lookup(cp):
        for c in cmaps:
            if c["start"] is None or not (c["start"] <= cp < c["start"] + c["length"]):
                continue
            off = cp - c["start"]
            if "SPARSE" in c["t"]:
                arr = lists.get(c["ul"], [])
                if off in arr:
                    return c["gid"] + arr.index(off)
                continue
            idx = off
            if c["gol"]:
                arr = ofs.get(c["gol"], [])
                if off >= len(arr):
                    continue
                idx = arr[off]
            return c["gid"] + idx
        return 0

    missing = "".join(c for c in chars if lookup(ord(c)) == 0)
    if missing:
        print(f"  !! 缺字 {len(missing)} 个：{missing}")
        return len(missing)
    print(f"  覆盖：清单 {len(chars)} 字全部命中")
    return 0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--font", required=True, help="源字体（TTF/OTF）")
    ap.add_argument("--chars", help="字符清单（gen/font_chars.txt）")
    ap.add_argument("--symbols",
                    help="直接用命令行给字符集，不用文件。用于只要几个字的"
                         "极小字体，例如血条框里的 HP 标签：--symbols HP。"
                         "ASCII 段由 --range 0x20-0x7F 另外带上，不用写进这里")
    ap.add_argument("--out", required=True, help="输出 .c 路径")
    ap.add_argument("--name", required=True, help="生成的 lv_font_t 变量名")
    ap.add_argument("--size", type=int, default=16, help="字号，默认 16")
    ap.add_argument("--no-subset", action="store_true",
                    help="跳过子集化（TTF 等能被 opentype.js 直读的字体可用）")
    args = ap.parse_args()

    font = Path(args.font)
    if not font.exists():
        sys.exit(f"源字体不存在：{font}")

    if args.symbols:
        chars = "".join(sorted(set(c for c in args.symbols if not c.isspace())))
        print(f"字符集（命令行给定）：{len(chars)} 字「{chars}」")
    elif args.chars:
        chars = read_chars(args.chars)
        print(f"字符清单：{len(chars)} 字")
    else:
        sys.exit("需要 --chars <清单文件> 或 --symbols <字符集>")
    print(f"源字体：{font.name}（{font.stat().st_size/1048576:.2f} MB）")

    tmpdir = None
    src = font
    if not args.no_subset:
        tmpdir = Path(tempfile.mkdtemp(prefix="fontsubset_"))
        sub = subset(font, chars, tmpdir / (font.stem + "-subset" + font.suffix))
        if sub:
            src = sub

    print("调 lv_font_conv ...")
    Path(args.out).parent.mkdir(parents=True, exist_ok=True)
    run_conv(src, chars, Path(args.out), args.name, args.size)

    print("自检：")
    bad = verify(Path(args.out), chars, 4, args.size)
    size_kb = Path(args.out).stat().st_size / 1024
    print(f"输出 {args.out}（源码 {size_kb:.0f} KB）")

    if tmpdir:
        shutil.rmtree(tmpdir, ignore_errors=True)
    sys.exit(1 if bad else 0)


if __name__ == "__main__":
    main()
