"""
make_readme_images.py - 生成 README 用的配图

产出（默认 docs/img/）：

  hero.png          战斗界面布局示意（240x320 原生分辨率，放大 3 倍）
  sprites.png       精灵图总览（三代火红/叶绿立绘 + Mega）
  font-compare.png  两套字库同文本对比（自制 1bpp vs LVGL 4bpp）

为什么用脚本画
--------------
三张图的内容全部来自**固件里的真实数据**：精灵图是 kSpriteTable 里解出来的
4bpp 像素，文字是两套字库真实渲染的。手画的话数据一改图就过期。

改完数据后重出：`python tools/make_readme_images.py`

hero.png 是**布局示意图**，不是真机截屏——渲染层还没写。

依赖 Pillow，以及已生成好的 gen/poke_sprites.c、gen/font_cn.c、
gen/font_app_16.c。
"""

import argparse
import re
import sys
from pathlib import Path

from PIL import Image, ImageDraw

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tools"))

from render_lvgl_font import LVGLFont  # noqa: E402

# ---------------------------------------------------------------- 调色

SKY_TOP = (74, 154, 224)
SKY_BOT = (176, 220, 250)
GROUND = (98, 172, 92)
PLATFORM = (168, 220, 148)

HPBOX_BG = (248, 248, 240)
HPBOX_EDGE = (40, 44, 52)
TEXT_DARK = (44, 44, 52)
TEXT_WARN = (192, 84, 44)

BAR_TRACK = (96, 96, 104)
BAR_GREEN = (104, 200, 96)
BAR_YELLOW = (240, 200, 72)
BAR_RED = (232, 88, 72)

PANEL_BG = (30, 34, 44)
PANEL_EDGE = (78, 86, 102)
TEXT_LIGHT = (234, 238, 246)
MOVE_BOX = (44, 50, 64)
MOVE_SEL = (62, 72, 96)
CURSOR = (255, 214, 92)

PAGE_BG = (18, 20, 26)
PAGE_TITLE = (140, 212, 255)
PAGE_SUB = (128, 140, 158)
LABEL = (206, 212, 224)

SCREEN_W, SCREEN_H = 240, 320


# ---------------------------------------------------------------- 1bpp 字库

class OneBppFont:
    """解析 gen/font_cn.h + .c（自制 1bpp 点阵字库）。

    在 Python 里独立再实现一遍解码，顺带是一次格式复核：行字节数必须用
    `FONT_STRIDE = ceil(CELL_W/8)`，**不是 `CELL_W // 8`**。当初正是这里
    用了整数除法（14//8 = 1），每行少写 6 个像素，和读取端的 ceil 对不上，
    字形全变成散点。
    """

    def __init__(self, hdr_path, src_path):
        hdr = Path(hdr_path).read_text(encoding="utf-8", errors="replace")
        self.w = int(re.search(r"#define FONT_CELL_W (\d+)", hdr).group(1))
        self.h = int(re.search(r"#define FONT_CELL_H (\d+)", hdr).group(1))
        self.stride = int(re.search(r"#define FONT_STRIDE (\d+)", hdr).group(1))

        src = Path(src_path).read_text(encoding="utf-8", errors="replace")
        m = re.search(r"kFontBitmap\[\d+\]\s*=\s*\{(.*?)\n\};", src, re.S)
        if not m:
            raise SystemExit("找不到 kFontBitmap")
        self.bitmap = bytes(int(x, 16) for x in
                            re.findall(r"0x([0-9A-Fa-f]{1,2})", m.group(1)))

        m = re.search(r"kFontMap\[\]\s*=\s*\{(.*?)\n\};", src, re.S)
        self.map = {int(a, 16): int(b) for a, b in
                    re.findall(r"\{\s*(0x[0-9A-Fa-f]+)\s*,\s*(\d+)\s*\}",
                               m.group(1))}

        need = (max(self.map.values()) + 1) * self.h * self.stride
        if need > len(self.bitmap):
            raise SystemExit(f"位图过短：需要 {need} 实际 {len(self.bitmap)}")

    def advance(self, ch):
        return self.w                      # 1bpp 是等宽全角

    def measure(self, text):
        return self.w * len(text)

    def draw(self, img, x, y, text, fg=(240, 244, 255)):
        px = img.load()
        W, H = img.size
        cx = x
        for ch in text:
            gi = self.map.get(ord(ch))
            if gi is not None:
                base = gi * self.h * self.stride
                for gy in range(self.h):
                    for gx in range(self.w):
                        bit = (self.bitmap[base + gy * self.stride + gx // 8]
                               >> (7 - gx % 8)) & 1
                        if not bit:
                            continue
                        dx, dy = cx + gx, y + gy
                        if 0 <= dx < W and 0 <= dy < H:
                            px[dx, dy] = fg
            cx += self.w
        return cx


# ---------------------------------------------------------------- 数据解析

def load_species_names(path):
    """{pid: (英文名, 中文名)}"""
    text = Path(path).read_text(encoding="utf-8", errors="replace")
    out = {}
    for m in re.finditer(r'\{\s*(\d+),\s*\d+,\s*"([^"]*)",\s*"([^"]*)"', text):
        out[int(m.group(1))] = (m.group(2), m.group(3))
    if not out:
        raise SystemExit("解析不到 kSpecies")
    return out


def _palette(text, name):
    m = re.search(rf"{name}\[16\]\s*=\s*\{{([^}}]*)\}}", text)
    if not m:
        raise SystemExit(f"找不到调色板 {name}")
    return [int(x, 16) for x in re.findall(r"0x[0-9A-Fa-f]{4}", m.group(1))]


def _rgb565(c):
    return (((c >> 11) & 0x1F) * 255 // 31,
            ((c >> 5) & 0x3F) * 255 // 63,
            (c & 0x1F) * 255 // 31)


def load_sprite(text, data_name, pal_name, w, h):
    """把一张 4bpp 索引色精灵图解成 RGBA 图。"""
    m = re.search(rf"{data_name}\[\]\s*=\s*\{{(.*?)\n\}};", text, re.S)
    if not m:
        raise SystemExit(f"找不到像素数组 {data_name}")
    data = bytes(int(x, 16) for x in
                 re.findall(r"0x([0-9A-Fa-f]{1,2})", m.group(1)))
    pal = [_rgb565(c) for c in _palette(text, pal_name)]

    need = w * h // 2                      # 4bpp：每字节 2 像素
    if len(data) < need:
        raise SystemExit(f"{data_name} 像素数据不足：{len(data)} < {need}")

    img = Image.new("RGBA", (w, h), (0, 0, 0, 0))
    px = img.load()
    for i in range(w * h):
        idx = (data[i // 2] >> (0 if i % 2 else 4)) & 0xF
        if idx == 0:
            continue                        # 索引 0 = 透明
        r, g, b = pal[idx]
        px[i % w, i // w] = (r, g, b, 255)
    return img


def sprite_table(text):
    """{id: {'front': (name, pal), 'mega': ...}}"""
    out = {}
    for m in re.finditer(
            r"\{\s*(\d+),\s*(SPR_\w+),\s*(spr_\w+),\s*(spr_\w+),\s*(\d+),\s*(\d+)\s*\}",
            text):
        pid, kind, dn, pn, w, h = (int(m.group(1)), m.group(2), m.group(3),
                                   m.group(4), int(m.group(5)), int(m.group(6)))
        k = "mega" if "MEGA" in kind else "front"
        out.setdefault(pid, {})[k] = dict(name=dn, pal=pn, w=w, h=h)
    if not out:
        raise SystemExit("解析不到 kSpriteTable")
    return out


# ---------------------------------------------------------------- 绘制小工具

def rect(d, box, fill=None, edge=None, width=1):
    d.rectangle(box, fill=fill, outline=edge, width=width)


def bar(d, x, y, w, h, ratio, color):
    d.rectangle([x, y, x + w - 1, y + h - 1], fill=BAR_TRACK)
    fill_w = max(0, min(w - 2, int((w - 2) * ratio)))
    if fill_w > 0:
        d.rectangle([x + 1, y + 1, x + 1 + fill_w - 1, y + h - 2], fill=color)


def hp_color(ratio):
    if ratio > 0.5:
        return BAR_GREEN
    if ratio > 0.2:
        return BAR_YELLOW
    return BAR_RED


def check_charset(font, strings, label):
    """
    断言所有要画的字符都在字库覆盖范围内。

    字库的字符是从**真实数据（六张表的中文名）与真实文案（引擎日志）**
    收集的，所以任何我临时编出来的中文都可能不在里面 —— 渲染出来是空白，
    不报错、不崩溃、图看着还挺正常。

    第一版 hero 图正是这么踩的：我写了「效果绝佳」，而引擎日志里根本没
    这个词，「佳」不在字库，图上就出现一个空档。这里把它变成硬失败。
    """
    missing = {}
    for s in strings:
        for c in s:
            if c.isspace():
                continue
            if font.glyph(c) is None:
                missing.setdefault(c, s)
    if missing:
        items = "、".join(f"「{c}」(出自「{s}」)" for c, s in missing.items())
        raise SystemExit(f"{label} 用了字库没有的字：{items}")
    return len(set("".join(strings)))


# ---------------------------------------------------------------- 三个图

def make_hero(f_lvgl, sprites, names, scale):
    """240x320 战斗界面布局示意。"""
    img = Image.new("RGB", (SCREEN_W, SCREEN_H), GROUND)
    d = ImageDraw.Draw(img)

    GROUND_Y = 96

    # 天空渐变
    for y in range(GROUND_Y):
        t = y / max(1, GROUND_Y - 1)
        d.line([(0, y), (SCREEN_W, y)],
               fill=tuple(int(a + (b - a) * t)
                          for a, b in zip(SKY_TOP, SKY_BOT)))
    # 地面
    d.rectangle([0, GROUND_Y, SCREEN_W, SCREEN_H], fill=GROUND)

    # 立绘平台（画在立绘与面板之前，会被它们盖住下缘）
    d.ellipse([146, 74, 226, 98], fill=PLATFORM)
    d.ellipse([4, 148, 100, 172], fill=PLATFORM)

    # 对手立绘（右上）。底边落在 y=82，**必须高于我方 HP 框的顶边 y=88**，
    # 否则会被 HP 框盖住下半身（第一版就是这个问题）。
    foe = sprites[("metagross", "front")]
    img.paste(foe, (150, 18), foe)
    # 我方立绘（左下）
    me = sprites[("charizard", "front")]
    img.paste(me, (14, 96), me)

    # ---- 对手 HP 框（左上）----
    d.rounded_rectangle([6, 8, 118, 46], radius=4,
                        fill=HPBOX_BG, outline=HPBOX_EDGE, width=2)
    f_lvgl.draw(img, 14, 13, names["metagross"], TEXT_DARK)
    lv = "Lv50"
    lv_w = f_lvgl.measure(lv)
    f_lvgl.draw(img, 108 - lv_w, 13, lv, TEXT_DARK)
    bar(d, 12, 35, 100, 7, 0.72, hp_color(0.72))

    # ---- 我方 HP 框（右下）----
    d.rounded_rectangle([118, 88, 234, 142], radius=4,
                        fill=HPBOX_BG, outline=HPBOX_EDGE, width=2)
    f_lvgl.draw(img, 126, 93, names["charizard"], TEXT_DARK)
    lv_w = f_lvgl.measure(lv)
    f_lvgl.draw(img, 226 - lv_w, 93, lv, TEXT_DARK)
    bar(d, 124, 115, 102, 7, 0.44, hp_color(0.44))
    f_lvgl.draw(img, 124, 124, "HP 68/153", TEXT_DARK)

    # ---- 下方菜单面板 ----
    d.rectangle([0, 164, SCREEN_W, SCREEN_H], fill=PANEL_BG)
    d.line([(0, 164), (SCREEN_W, 164)], fill=PANEL_EDGE)

    # 战斗日志两行。**用 src/poke_battle.c 里的原文**，不要自己编词：
    # 字库的字符是从真实文案收集的，编出来的词（如「效果绝佳」的「佳」）
    # 不在字库里，会渲染成空白。校验见 check_charset()。
    f_lvgl.draw(img, 10, 168, "喷火龙 使用了 喷射火焰！", TEXT_LIGHT)
    f_lvgl.draw(img, 10, 189, "会心一击！造成了 187 点伤害", TEXT_LIGHT)
    d.line([(0, 212), (SCREEN_W, 212)], fill=PANEL_EDGE)

    # 招式菜单 2x2。左右两列各 114 宽：名字最长 4 个汉字（52px），
    # PP 最长 "15/15"（40px），留出光标与间隙后正好不重叠。
    moves = [("喷射火焰", "15/15"), ("龙爪", "15/15"),
             ("翅膀攻击", "35/35"), ("剑舞", "20/20")]
    for i, (nm, pp) in enumerate(moves):
        col, row = i % 2, i // 2
        x = 4 + col * 118
        y = 218 + row * 30
        sel = (i == 0)
        d.rounded_rectangle([x, y, x + 114, y + 26], radius=3,
                            fill=MOVE_SEL if sel else MOVE_BOX,
                            outline=CURSOR if sel else PANEL_EDGE)
        ink = CURSOR if sel else TEXT_LIGHT
        if sel:
            d.polygon([(x + 4, y + 9), (x + 4, y + 17), (x + 10, y + 13)],
                      fill=CURSOR)
        f_lvgl.draw(img, x + 13, y + 3, nm, ink)
        pp_w = f_lvgl.measure(pp)
        f_lvgl.draw(img, x + 110 - pp_w, y + 3, pp,
                    CURSOR if sel else PAGE_SUB)

    # 底部提示条（用词取自 tools/gen_font.py 的 UI_STRINGS）
    d.line([(0, 278), (SCREEN_W, 278)], fill=PANEL_EDGE)
    f_lvgl.draw(img, 10, 282, "选择招式   换人   逃跑   返回", LABEL)

    return img.resize((SCREEN_W * scale, SCREEN_H * scale), Image.NEAREST)


def make_sprites(font, sprites, names, scale=2, cols=5, pad=14, label_h=30):
    items = [("charizard", "front"), ("pikachu", "front"),
             ("mewtwo", "front"), ("scizor", "front"),
             ("blastoise", "front"), ("gyarados", "front"),
             ("alakazam", "front"), ("exeggutor", "front"),
             ("metagross", "front"), ("charizard_mega_x", "mega")]

    # Veekun 里 Mega 形态的中文名与基础形态**完全相同**（venusaur_mega 也叫
    # 「妙蛙花」），直接显示会分不清，所以 Mega 额外加个后缀。
    suffix = {"mega": " (MegaX)"}

    cell_w = 64 * scale
    cell_h = 64 * scale + label_h
    rows = (len(items) + cols - 1) // cols
    W = cols * cell_w + (cols + 1) * pad
    title_h = 56
    H = title_h + rows * cell_h + (rows + 1) * pad

    img = Image.new("RGB", (W, H), PAGE_BG)
    d = ImageDraw.Draw(img)
    # 标题是 ASCII，用 PIL 默认字体即可
    d.text((pad + 2, 14), "SPRITES   427 front + Mega   4bpp indexed color",
           fill=PAGE_TITLE)
    d.text((pad + 2, 32), "generation-iii/firered-leafgreen, native 64x64",
           fill=PAGE_SUB)

    labels = []
    for i, (key, kind) in enumerate(items):
        col, row = i % cols, i // cols
        x = pad + col * (cell_w + pad)
        y = title_h + pad + row * (cell_h + pad)
        s = sprites[(key, kind)]
        big = s.resize((cell_w, cell_w), Image.NEAREST)
        img.paste(big, (x, y), big)
        nm = names.get(key, key) + suffix.get(kind, "")
        # 标签用固件字库画 —— PIL 默认字体没有中文字形，会渲染成方块
        lx = x + (cell_w - font.measure(nm)) // 2
        labels.append(nm)
        font.draw(img, lx, y + cell_w + 4, nm, LABEL)

    check_charset(font, labels, "sprites.png 标签")
    return img


def make_font_compare(f_lvgl, f_1bpp, texts, scale=4):
    lines = []
    for t in texts:
        w1, w2 = f_1bpp.measure(t), f_lvgl.measure(t)
        lines.append((t, w1, w2))

    pad, lab = 16, 74
    body_w = max(max(a, b) for _, a, b in lines)
    W = lab + body_w + pad
    row_h = 30
    H = 58 + len(lines) * row_h * 2 + 20

    img = Image.new("RGB", (W, H), PAGE_BG)
    d = ImageDraw.Draw(img)
    d.text((pad, 12), "FONT COMPARE  1bpp bitmap vs LVGL 4bpp", fill=PAGE_TITLE)
    d.text((pad, 30), "same text, same source size", fill=PAGE_SUB)

    y = 54
    for t, _, _ in lines:
        d.text((pad, y + 6), "1bpp", fill=PAGE_SUB)
        f_1bpp.draw(img, lab, y, t)
        y += row_h
        d.text((pad, y + 6), "4bpp", fill=PAGE_SUB)
        f_lvgl.draw(img, lab, y, t)
        y += row_h

    return img.resize((W * scale, H * scale), Image.NEAREST)


# ---------------------------------------------------------------- 入口

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", default="docs/img")
    ap.add_argument("--scale", type=int, default=3, help="hero 放大倍数")
    ap.add_argument("--only", default="", help="只生成某一项：hero/sprites/font")
    args = ap.parse_args()

    out = ROOT / args.out
    out.mkdir(parents=True, exist_ok=True)

    f_lvgl = LVGLFont(ROOT / "gen/font_app_16.c", bpp=4)
    print(f"LVGL 字体：字形 {len(f_lvgl.glyphs)}，"
          f"line_height={f_lvgl.line_height} base_line={f_lvgl.base_line}")

    names_cn = load_species_names(ROOT / "gen/poke_species.c")
    # 英文标识符 -> 中文名
    en2cn = {en: cn for _, (en, cn) in names_cn.items()}
    print(f"一个汉字约 {f_lvgl.measure('妙蛙种子') / 4:.1f} px 宽，"
          f"ASCII 约 {f_lvgl.measure('A')} px")

    task = args.only or "all"

    # ---- 先校验所有要画的中文都在字库里 ----
    # 日志文案必须与 src/poke_battle.c 的 log_add 原文一致；UI 用词必须
    # 出自 tools/gen_font.py 的 UI_STRINGS。写错一个字就硬失败。
    hero_strings = [
        "巨金怪", "喷火龙", "Lv50", "HP 68/153",
        "喷火龙 使用了 喷射火焰！", "会心一击！造成了 187 点伤害",
        "喷射火焰", "龙爪", "翅膀攻击", "剑舞",
        "15/15", "35/35", "20/20",
        "选择招式   换人   逃跑   返回",
    ]
    n = check_charset(f_lvgl, hero_strings, "hero.png")
    print(f"hero 文案用字 {n} 个，字库全部覆盖")

    sprite_src = (ROOT / "gen/poke_sprites.c").read_text(
        encoding="utf-8", errors="replace")
    table = sprite_table(sprite_src)

    want = [("charizard", "front"), ("pikachu", "front"), ("mewtwo", "front"),
            ("scizor", "front"), ("blastoise", "front"), ("gyarados", "front"),
            ("alakazam", "front"), ("exeggutor", "front"),
            ("metagross", "front"), ("charizard_mega_x", "mega")]
    sprites = {}
    for en, kind in want:
        pid = next((k for k, (e, _) in names_cn.items() if e == en), None)
        if pid is None:
            raise SystemExit(f"找不到宝可梦 {en}")
        info = table.get(pid, {}).get(kind)
        if info is None:
            print(f"  跳过 {en} ({kind})：表中没有")
            continue
        sprites[(en, kind)] = load_sprite(sprite_src, info["name"],
                                         info["pal"], info["w"], info["h"])
    print(f"已解出 {len(sprites)} 张精灵图")

    if task in ("all", "hero"):
        hero = make_hero(f_lvgl, sprites, en2cn, args.scale)
        hero.save(out / "hero.png")
        print(f"hero.png          {hero.width}x{hero.height}")

    if task in ("all", "sprites"):
        sheet = make_sprites(f_lvgl, sprites, en2cn)
        sheet.save(out / "sprites.png")
        print(f"sprites.png       {sheet.width}x{sheet.height}")

    if task in ("all", "font"):
        f1 = OneBppFont(ROOT / "gen/font_cn.h", ROOT / "gen/font_cn.c")
        cmp_img = make_font_compare(
            f_lvgl, f1,
            ["妙蛙种子 喷火龙 巨钳螳螂", "喷射火焰 十万伏特 Lv50"])
        cmp_img.save(out / "font-compare.png")
        print(f"font-compare.png  {cmp_img.width}x{cmp_img.height}")


if __name__ == "__main__":
    main()
