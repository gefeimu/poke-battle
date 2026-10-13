"""
render_lvgl_font.py - 把 lv_font_conv 生成的 LVGL 字体渲染成图片

为什么需要
----------
字库的 bug 全是「安静地错」：能编译、能查表、大小也正常，但画出来是空白或
错位。我们的 1bpp 自制字库就踩过一次（行字节数用了整数除法，字形变散点）。
所以换成 LVGL 字体后必须同样能**导图肉眼验证**，不能只看文件生成了。

LVGL 字体 .c 的结构（4bpp、--no-compress 时）
--------------------------------------------
  glyph_bitmap   逐字形位图，行优先，每字节 2 像素（高半字节在前）
  glyph_dsc      每字形的 {bitmap_index, adv_w, box_w, box_h, ofs_x, ofs_y}
  cmaps          码点 -> 字形索引的映射（ASCII 段用紧凑格式，CJK 用稀疏列表）
  unicode_list   稀疏 cmap 的码点偏移表

用法：
    python tools/render_lvgl_font.py \\
        --font gen/font_app_16.c \\
        --name font_app_16 \\
        --text "妙蛙种子 喷火龙" \\
        --out data/font_preview/lvgl_font.png
"""

import argparse
import re
import sys
from pathlib import Path

from PIL import Image, ImageDraw

ROOT = Path(__file__).resolve().parent.parent


# ---------------------------------------------------------------- 解析

def _strip_braces(s):
    return s.replace("{", " ").replace("}", " ")


def parse_glyph_bitmap(text):
    """
    解析 glyph_bitmap 数组。

    **hex 位数必须写成 {1,2}，不能写 {2}。**
    生成器对小于 0x10 的字节写的是单个十六进制位（`0x0`、`0x9`、`0xf`），
    只匹配两位会把所有 0x00-0x0F 的字节整批丢掉 —— 症状是位图长度明显偏短
    （实测 1721 字形只剩 105 KB，实际应为 162 KB），解出来的字形是像素噪声。
    """
    m = re.search(r"glyph_bitmap\[\]\s*=\s*\{(.*?)\n\};", text, re.S)
    if not m:
        raise SystemExit("找不到 glyph_bitmap")
    return bytes(int(x, 16) for x in re.findall(r"0x([0-9A-Fa-f]{1,2})", m.group(1)))


def verify_bitmap(bitmap, glyphs, bpp):
    """
    自检：位图总长应等于各字形长度之和（连续位打包）。

    这条断言能一次抓出两类错误：hex 解析漏字节、字形长度公式写错。
    返回 (期望长度, 实际长度, 是否一致)。
    """
    total = 0
    for g in glyphs:
        total += -(-g["box_w"] * g["box_h"] * bpp // 8)
    return total, len(bitmap), total == len(bitmap)


def parse_glyph_dsc(text):
    """返回 [{bitmap_index, adv_w, box_w, box_h, ofs_x, ofs_y}, ...]"""
    m = re.search(r"glyph_dsc\[\]\s*=\s*\{(.*?)\n\};", text, re.S)
    if not m:
        raise SystemExit("找不到 glyph_dsc")
    body = m.group(1)
    out = []
    for ent in re.finditer(r"\{([^{}]*)\}", body):
        f = _strip_braces(ent.group(1))
        kv = {}
        for part in f.split(","):
            part = part.strip()
            if "=" in part:
                k, v = part.split("=", 1)
                kv[k.strip().lstrip(".")] = int(v.strip(), 0)
        if kv:
            out.append(kv)
    return out


def parse_cmaps(text):
    """解析 cmaps 与它引用的两个辅助表。

    返回 (cmap 列表, unicode_list 字典, glyph_id_ofs_list 字典)。

    注意 LVGL 的 cmap 类型有两层含义，容易看错：
      FORMAT0_*  连续范围，字形索引 = glyph_id_start + (cp - range_start)
                 FULL 变体再用 glyph_id_ofs_list 做二次偏移（用于只覆盖
                 部分码点的连续段）
      SPARSE_*   稀疏，字符表在 unicode_list（值是 cp - range_start），
                 下标即字形序号
    后缀 TINY/FULL 只表示**偏移表元素宽度**（uint8 / uint16），
    不代表稀不稀疏。曾经的实现按 "TINY" 判断连续性，
    把 SPARSE_TINY 当成连续范围，算出的字形索引超出字形总数（22800 > 1721）。
    """
    lists = {}
    for m in re.finditer(r"unicode_list(_\d+)?\[\]\s*=\s*\{(.*?)\n\};", text, re.S):
        name = "unicode_list" + (m.group(1) or "")
        lists[name] = [int(x, 0) for x in re.findall(r"0x[0-9A-Fa-f]+|\d+", m.group(2))]

    ofs_lists = {}
    for m in re.finditer(r"glyph_id_ofs_list(_\d+)?\[\]\s*=\s*\{(.*?)\n\};", text, re.S):
        name = "glyph_id_ofs_list" + (m.group(1) or "")
        ofs_lists[name] = [int(x, 0) for x in re.findall(r"0x[0-9A-Fa-f]+|\d+", m.group(2))]

    m = re.search(r"cmaps\[\]\s*=\s*\{(.*?)\n\};", text, re.S)
    if not m:
        raise SystemExit("找不到 cmaps")
    body = m.group(1)
    out = []
    for ent in re.finditer(r"\{([^{}]*(?:\{[^{}]*\}[^{}]*)*)\}", body, re.S):
        f = ent.group(1)

        def get(key, default=None):
            mm = re.search(rf"\.{key}\s*=\s*([^,}}]+)", f)
            if not mm:
                return default
            v = mm.group(1).strip()
            if v == "NULL":
                return None
            if v.startswith(("unicode_list", "glyph_id_ofs_list")):
                return v
            try:
                return int(v, 0)
            except ValueError:
                return v

        out.append(dict(
            range_start=get("range_start", 0),
            range_length=get("range_length", 0),
            glyph_id_start=get("glyph_id_start", 0),
            unicode_list=get("unicode_list"),
            glyph_id_ofs_list=get("glyph_id_ofs_list"),
            list_length=get("list_length", 0),
            type=str(get("type", "")),
        ))
    return out, lists, ofs_lists


def lookup_glyph(cmaps, lists, ofs_lists, cp):
    """按 LVGL 的查找逻辑返回字形索引，找不到返回 0（.notdef）。"""
    for cm in cmaps:
        t = str(cm["type"])
        start, length = cm["range_start"], cm["range_length"]
        if start > cp or cp >= start + length:
            continue
        off = cp - start

        if "SPARSE" in t:                       # 稀疏：查字符表
            ul = cm["unicode_list"]
            if not ul:
                continue
            arr = lists.get(ul, [])
            try:
                pos = arr.index(off)
            except ValueError:
                continue
            return cm["glyph_id_start"] + pos

        # 连续：FULL 还要过一遍偏移表
        idx = off
        gol = cm["glyph_id_ofs_list"]
        if gol:
            arr = ofs_lists.get(gol, [])
            if off >= len(arr):
                continue
            idx = arr[off]
        return cm["glyph_id_start"] + idx
    return 0


# ---------------------------------------------------------------- 渲染

def parse_font_metrics(text):
    """取 line_height / base_line，用于正确摆放基线。"""
    m = re.search(r"\.line_height\s*=\s*(\d+)", text)
    lh = int(m.group(1)) if m else 0
    m = re.search(r"\.base_line\s*=\s*(-?\d+)", text)
    bl = int(m.group(1)) if m else 0
    return lh, bl


def render(text, cmaps, lists, ofs_lists, glyphs, bitmap, bpp=4,
           line_height=0, base_line=0, bg=(24, 28, 36),
           fg=(240, 244, 255), scale=2, pad=8):
    """
    逐字形合成一张图。

    纵向定位：LVGL 里 ofs_y 是从**基线**到字形包围盒顶部的偏移（向上为正），
    而基线距行顶 = line_height - base_line。所以
        行内顶部 y = (line_height - base_line) - ofs_y
    再用 box_h 向下铺。
    实测校验：'A' box_h=14 ofs_y=-1，line_height=21 base_line=5
        -> 顶部 y = 16 + 1 = 17，底边 17+14 = 31
    这不对（超出 21）。改用「底边贴基线」的读法：
        底边 y = (line_height - base_line) - ofs_y = 16 + 1 = 17
        顶部 y = 17 - 14 = 3  ->  占 3..17，正好落在行内且底部贴基线
    即 ofs_y 是**底边**相对基线的偏移。下面按这个实现。
    """
    baseline = (line_height - base_line) if line_height else 16
    if line_height <= 0:
        line_height = 21

    items = []
    total_w = 0
    for ch in text:
        gi = lookup_glyph(cmaps, lists, ofs_lists, ord(ch))
        if gi <= 0 or gi >= len(glyphs):
            items.append((None, 16))
            total_w += 16
            continue
        g = glyphs[gi]
        adv = max(1, round(g.get("adv_w", 16) / 16))  # adv_w 是像素的 16 倍
        items.append((g, adv))
        total_w += adv

    W = total_w + pad * 2
    H = line_height + pad * 2
    img = Image.new("RGB", (W, H), bg)
    px = img.load()

    # 读第 idx 个像素（idx = y*box_w + x）的亮度（0..1）。
    #
    # 关键：LVGL 的字形位图是**连续位打包**，行与行之间不补齐字节。
    # 一个 box_w×box_h 的 4bpp 字形占 ceil(box_w*box_h*4/8) 字节。
    # 曾按「每行字节对齐」解（stride = ceil(box_w*4/8)），
    # 结果除 box_w 为偶数外全部错位，字形渲染成像素噪声。
    # 实测 40 个连续字形全部符合连续打包（见 README 踩坑记录）。
    def sample(g, bi, x, y):
        idx = y * g["box_w"] + x
        if bpp == 4:
            pos = bi + idx // 2
            if pos >= len(bitmap):
                return 0.0
            b = bitmap[pos]
            return ((b >> 4) if idx % 2 == 0 else (b & 0x0F)) / 15.0
        if bpp == 8:
            pos = bi + idx
            return (bitmap[pos] / 255.0) if pos < len(bitmap) else 0.0
        if bpp == 1:
            pos = bi + idx // 8
            if pos >= len(bitmap):
                return 0.0
            return 1.0 if (bitmap[pos] >> (7 - idx % 8)) & 1 else 0.0
        # 2bpp
        pos = bi + idx // 4
        if pos >= len(bitmap):
            return 0.0
        b = bitmap[pos]
        return ((b >> (6 - (idx % 4) * 2)) & 3) / 3.0

    cx = pad
    for g, adv in items:
        if g is not None:
            bw, bh = g["box_w"], g["box_h"]
            if bw and bh:
                bi = g["bitmap_index"]
                ox = cx + g.get("ofs_x", 0)
                oy = pad + baseline - g.get("ofs_y", 0) - bh
                for y in range(bh):
                    for x in range(bw):
                        alpha = sample(g, bi, x, y)
                        if alpha <= 0:
                            continue
                        dx, dy = ox + x, oy + y
                        if 0 <= dx < W and 0 <= dy < H:
                            r, gg, bb = bg
                            px[dx, dy] = (
                                int(r + (fg[0] - r) * alpha),
                                int(gg + (fg[1] - gg) * alpha),
                                int(bb + (fg[2] - bb) * alpha),
                            )
        cx += adv

    if scale != 1 and scale > 0:
        img = img.resize((W * scale, H * scale), Image.NEAREST)
    return img


class LVGLFont:
    """
    可复用的绘制接口：把字体数据装进来，然后在任意 PIL 图上按坐标画字。

    命令行只出「一行文字一张图」，做界面示意图时不够用，所以把渲染逻辑
    抽成这个类，`tools/make_readme_images.py` 用它拼战斗界面 mockup。
    """

    def __init__(self, path, bpp=4):
        self.path = Path(path)
        text = self.path.read_text(encoding="utf-8", errors="replace")
        self.bitmap = parse_glyph_bitmap(text)
        self.glyphs = parse_glyph_dsc(text)
        self.cmaps, self.lists, self.ofs_lists = parse_cmaps(text)
        self.line_height, self.base_line = parse_font_metrics(text)
        self.bpp = bpp
        want, got, ok = verify_bitmap(self.bitmap, self.glyphs, bpp)
        if not ok:
            raise SystemExit(
                f"位图长度自检失败：期望 {want:,} 实际 {got:,}")

    def glyph(self, ch):
        gi = lookup_glyph(self.cmaps, self.lists, self.ofs_lists, ord(ch))
        if gi <= 0 or gi >= len(self.glyphs):
            return None
        return self.glyphs[gi]

    def advance(self, ch):
        """字符前进宽度（像素）。adv_w 在文件里是像素的 16 倍。"""
        g = self.glyph(ch)
        if g is None:
            return 16
        return max(1, round(g.get("adv_w", 16) / 16))

    def measure(self, text):
        return sum(self.advance(c) for c in text)

    def sample(self, g, x, y):
        """读第 (x,y) 个像素的亮度 0..1。位图是连续位打包，行间不补齐。"""
        bpp = self.bpp
        idx = y * g["box_w"] + x
        bi = g["bitmap_index"]
        bm = self.bitmap
        if bpp == 4:
            pos = bi + idx // 2
            if pos >= len(bm):
                return 0.0
            b = bm[pos]
            return ((b >> 4) if idx % 2 == 0 else (b & 0x0F)) / 15.0
        if bpp == 8:
            pos = bi + idx
            return (bm[pos] / 255.0) if pos < len(bm) else 0.0
        if bpp == 1:
            pos = bi + idx // 8
            if pos >= len(bm):
                return 0.0
            return 1.0 if (bm[pos] >> (7 - idx % 8)) & 1 else 0.0
        pos = bi + idx // 4
        if pos >= len(bm):
            return 0.0
        b = bm[pos]
        return ((b >> (6 - (idx % 4) * 2)) & 3) / 3.0

    def draw(self, img, x, y, text, fg=(240, 244, 255)):
        """
        在 img 的 (x, y) 处画一行文本，(x,y) 是**行框左上角**。
        返回画完后的 x（便于连续追加）。
        """
        px = img.load()
        W, H = img.size
        # 基线在行框内的位置；ofs_y 是字形底边相对基线的偏移
        baseline = y + ((self.line_height - self.base_line)
                        if self.line_height else 16)
        cx = x
        for ch in text:
            g = self.glyph(ch)
            if g is not None:
                bw, bh = g["box_w"], g["box_h"]
                if bw and bh:
                    ox = cx + g.get("ofs_x", 0)
                    oy = baseline - g.get("ofs_y", 0) - bh
                    for gy in range(bh):
                        for gx in range(bw):
                            a = self.sample(g, gx, gy)
                            if a <= 0:
                                continue
                            dx, dy = ox + gx, oy + gy
                            if not (0 <= dx < W and 0 <= dy < H):
                                continue
                            r, gg, bb = px[dx, dy]
                            px[dx, dy] = (
                                int(r + (fg[0] - r) * a),
                                int(gg + (fg[1] - gg) * a),
                                int(bb + (fg[2] - bb) * a),
                            )
            cx += self.advance(ch)
        return cx


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--font", required=True, help="lv_font_conv 生成的 .c 文件")
    ap.add_argument("--text", required=True, help="要渲染的文本")
    ap.add_argument("--out", required=True, help="输出 PNG 路径")
    ap.add_argument("--bpp", type=int, default=4)
    ap.add_argument("--scale", type=int, default=3)
    args = ap.parse_args()

    text_src = Path(args.font).read_text(encoding="utf-8", errors="replace")
    bitmap = parse_glyph_bitmap(text_src)
    glyphs = parse_glyph_dsc(text_src)
    cmaps, lists, ofs_lists = parse_cmaps(text_src)
    lh, bl = parse_font_metrics(text_src)

    print(f"字形 {len(glyphs)} 个，位图 {len(bitmap):,} 字节，"
          f"cmap {len(cmaps)} 段，line_height={lh} base_line={bl}")

    # 自检：位图长度必须与字形尺寸之和一致。曾经 hex 正则只匹配两位，
    # 丢掉所有 0x00-0x0F 的字节，位图长度短了三分之一却没人发现，
    # 渲染出来只是「噪声」而非报错。
    want, got, ok = verify_bitmap(bitmap, glyphs, args.bpp)
    if not ok:
        print(f"!! 位图长度自检失败：期望 {want:,} 实际 {got:,}"
              f"（差 {got - want:+,}）")
        sys.exit(1)
    print("位图长度自检通过")

    missing = [c for c in args.text
               if lookup_glyph(cmaps, lists, ofs_lists, ord(c)) == 0]
    if missing:
        print(f"缺字 {len(missing)} 个: {''.join(missing)}")

    img = render(args.text, cmaps, lists, ofs_lists, glyphs, bitmap,
                 bpp=args.bpp, line_height=lh, base_line=bl,
                 scale=args.scale)
    Path(args.out).parent.mkdir(parents=True, exist_ok=True)
    img.save(args.out)
    print(f"已导出 {args.out}  ({img.width}x{img.height})")


if __name__ == "__main__":
    main()
