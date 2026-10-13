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
#
# 下面这组颜色不是随手挑的，是从 FoloToy 该项目对应的三代原版
# （火红/叶绿）的反编译仓里取出的真实调色板：
#
#   graphics/battle_interface/healthbox.pal   血条框
#   graphics/battle_interface/healthbar.pal   HP 条
#   graphics/battle_interface/textbox1.pal    对话框
#
# 顺带拿到了血条框的真实尺寸（OAM 定义 = 64x64 两个精灵拼合）：
#   对手框 128x32，我方框 128x64（多一条 EXP 条）

# 血条框
HB_FILL = (255, 255, 222)     # healthbox.pal[2] 米白填充
HB_EDGE = (65, 65, 65)        # healthbox.pal[1] 边框
HB_INNER = (222, 213, 180)    # healthbox.pal[3] 内斜角
HB_TEXT = (32, 57, 0)         # healthbox.pal[7] 框内文字（深绿）
HB_SHADOW = (82, 106, 98)     # healthbox.pal[6] 右下投影（墨绿）
# healthbox.pal[11] 是原版 EXP 条的青色，本项目不用 EXP 条，故不定义

# HP 条（双色立体：上行高光 + 下行基色）
BAR_TRACK = (57, 57, 57)
BAR_TRACK_HI = (131, 131, 139)
BAR_GREEN = (115, 255, 172), (90, 213, 131)
BAR_YELLOW = (255, 230, 57), (205, 172, 8)
BAR_RED = (255, 90, 57), (172, 65, 74)

# 对话框（白底 + 蓝框）
DLG_FILL = (255, 255, 255)
DLG_EDGE = (65, 74, 123)
DLG_EDGE_HI = (106, 139, 189)
DLG_TEXT = (41, 49, 49)
DLG_GOLD = (156, 123, 24)

# 选中项高亮
SEL_FILL = (255, 230, 57)
SEL_EDGE = (205, 172, 8)
SEL_TEXT = (41, 49, 49)

# 场景
SKY_TOP = (108, 176, 232)
SKY_BOT = (196, 226, 248)
GROUND = (152, 200, 112)
GROUND_DARK = (120, 176, 88)
PLATFORM = (184, 220, 148)
PLATFORM_DARK = (144, 192, 108)

# 页面底（README 配图的画布，不属于游戏本体）
PAGE_BG = (18, 20, 26)
PAGE_TITLE = (140, 212, 255)
PAGE_SUB = (128, 140, 158)
LABEL = (206, 212, 224)

SCREEN_W, SCREEN_H = 240, 320
BATTLE_H = 160                      # 上半屏 = 原版 GBA 战斗画面高度


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

def hp_color(ratio):
    """FRLG 的 HP 条分三档变色，返回 (高光色, 基色)。"""
    if ratio > 0.5:
        return BAR_GREEN
    if ratio > 0.2:
        return BAR_YELLOW
    return BAR_RED


def hp_bar(d, x, y, w, h, ratio):
    """
    画 FRLG 风格的 HP 条。

    两个要点：
      1. **双色立体**——原版不是单色矩形，而是上行高光、下行基色，外面套深色槽。
         按单色画会明显「不像」。
      2. **四角圆角**——两端收成胶囊形，与血条框的圆角风格统一。
         原版是方角，这里按需求改成圆角。
    """
    r = max(1, h // 2)
    d.rounded_rectangle([x, y, x + w - 1, y + h - 1], radius=r, fill=BAR_TRACK)

    inner_w = w - 2
    fill_w = max(0, min(inner_w, int(round(inner_w * ratio))))
    if fill_w < 3:
        return

    hi, lo = hp_color(ratio)
    x1, y1 = x + 1, y + 1
    x2, y2 = x + fill_w, y + h - 2
    r2 = max(1, (h - 2) // 2)
    d.rounded_rectangle([x1, y1, x2, y2], radius=r2, fill=lo)
    # 上缘高光：两端各收进 2px，避免方角从胶囊形的圆弧里戳出来
    if fill_w > 5:
        d.rectangle([x1 + 2, y1, x2 - 2, y1 + max(0, h // 3 - 1)], fill=hi)


def frlg_frame(d, x, y, w, h, radius=5):
    """FRLG 血条框的外框：右下角墨绿投影 + 深灰描边 + 米白填充。"""
    d.rounded_rectangle([x + 3, y + 3, x + w - 1 + 3, y + h - 1 + 3],
                        radius=radius, fill=HB_SHADOW)
    d.rounded_rectangle([x, y, x + w - 1, y + h - 1],
                        radius=radius, fill=HB_FILL, outline=HB_EDGE, width=2)


HP_BAR_H = 5        # 条高（原版约 8，按需求调到 5）


def healthbox(font, img, d, x, y, w, h, name, lv, ratio, hp_text=None):
    """
    一个完整的血条框。`font` 传 12px 那套（16px 在小框里明显偏大）。

    对手框 128x32：名字 + Lv + HP 条（原版对手框不显示数字）
    我方框 128x48：多「HP」标签和一行 HP 数字

    **我方框高度与原版不同**：原版的 128x64 是因为最下面压着一条 EXP 条，
    本项目不用 EXP，所以压到 48——三行（名字 / HP 条 / 数字）正好填满，
    不然下方会留一块空白。
    """
    frlg_frame(d, x, y, w, h)

    if hp_text:
        name_y, bar_y, num_y = 3, 20, 31
    else:
        name_y, bar_y, num_y = 3, 20, 0
    font.draw(img, x + 8, y + name_y, name, HB_TEXT)          # 名字
    lv_x = x + w - 8 - font.measure(lv)
    font.draw(img, lv_x, y + name_y, lv, HB_TEXT)             # 等级右对齐

    if hp_text:
        # 我方框：左边标「HP」，条相应缩短；标签与条垂直居中对齐
        font.draw(img, x + 8, y + bar_y - 5, "HP", HB_TEXT)
        bar_x = x + 8 + font.measure("HP") + 5
    else:
        bar_x = x + 8
    hp_bar(d, bar_x, y + bar_y, (x + w - 8) - bar_x, HP_BAR_H, ratio)

    if hp_text:
        font.draw(img, x + w - 8 - font.measure(hp_text), y + num_y,
                  hp_text, HB_TEXT)


def dialog(d, x, y, w, h, radius=6):
    """FRLG 对话框：白底 + 深蓝外框 + 浅蓝内框。"""
    d.rounded_rectangle([x, y, x + w - 1, y + h - 1],
                        radius=radius, fill=DLG_FILL, outline=DLG_EDGE, width=3)
    d.rounded_rectangle([x + 3, y + 3, x + w - 4, y + h - 4],
                        radius=radius - 2, outline=DLG_EDGE_HI, width=1)


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

def make_hero(f_ui, f_lvgl, sprites, names, scale):
    """
    240x320 战斗界面，**按三代（火红/叶绿）原版版式重画**。

    版式依据（都从 pret/pokefirered 反编译仓取得，不是凭印象）：

    | 项 | 值 | 出处 |
    |---|---|---|
    | 对手血条框 | 128x32，左上角贴边 (0,0) | OAM `SPRITE_SHAPE(64x64)` x2 精灵拼合 |
    | 我方血条框 | 128x48，右下角贴边 (112,104) | 原版为 128x64，本项目去掉 EXP 条后压到 48 |
    | 网格 | 左上 / 右下**对角**摆放 | 原版就是对角，两框各占半屏宽 |
    | 框体配色 | 米白填充 + 深灰描边 + 右下墨绿投影 | `healthbox.pal` |
    | HP 条 | 双色立体（上行高光、下行基色）+ 深色槽 | `healthbar.pal` |
    | 对话框 | 白底 + 深蓝外框 + 浅蓝内框 | `textbox1.pal` |

    屏幕是 240x320，正好是 GBA 240x160 的两倍，所以：
      上半屏 y=0..159  = 原版战斗画面（含两个血条框）
      下半屏 y=160..319 = 对话框（日志）+ 招式菜单 + 状态行

    原版把对话框压在画面底部的 48px 里，所以血条框和对话框是挤在一起的；
    我们高一倍，正好可以把对话框整块放到下半屏，两框都完整可见。
    """
    img = Image.new("RGB", (SCREEN_W, SCREEN_H), GROUND_DARK)
    d = ImageDraw.Draw(img)

    HORIZON = 84

    # ---- 天空 + 地面（铺满整屏，对话框浮在上面）----
    # 原版的画面只有 240x160，我们的屏高一倍，所以场景直接铺满整屏，
    # 对话框叠在下半屏。比「下半屏另起一块纯色面板」更接近原版观感。
    for y in range(HORIZON):
        t = y / max(1, HORIZON - 1)
        d.line([(0, y), (SCREEN_W, y)],
               fill=tuple(int(a + (b - a) * t)
                          for a, b in zip(SKY_TOP, SKY_BOT)))
    for y in range(HORIZON, SCREEN_H):
        t = (y - HORIZON) / max(1, SCREEN_H - HORIZON - 1)
        d.line([(0, y), (SCREEN_W, y)],
               fill=tuple(int(a + (b - a) * t)
                          for a, b in zip(GROUND, GROUND_DARK)))

    # ---- 立绘与站台 ----
    # 对手：右上。64x64 画布的下边界在 y=86，必须**高于我方血条框顶边 y=88**，
    # 否则下半身会被血条框盖住（第一版踩过）。
    foe = sprites[("metagross", "front")]
    me = sprites[("charizard", "front")]
    FOE_XY, ME_XY = (146, 22), (10, 70)

    # 站台要按**可见像素**的下边界摆，不能按 64x64 画布的下边界：
    # 立绘画布底部通常留一圈空白（巨金怪留了 6px），按画布摆会让站台
    # 明显悬在脚底下。这是实际看预览图之后才发现的。
    foe_feet = FOE_XY[1] + foe.getbbox()[3]
    me_feet = ME_XY[1] + me.getbbox()[3]

    def platform(cx, feet_y, rx, ry):
        d.ellipse([cx - rx, feet_y - ry + 2, cx + rx, feet_y + ry + 2],
                  fill=PLATFORM_DARK)
        d.ellipse([cx - rx + 3, feet_y - ry, cx + rx - 3, feet_y + ry],
                  fill=PLATFORM)

    platform(178, foe_feet, 40, 9)
    platform(42, me_feet, 44, 10)

    img.paste(foe, FOE_XY, foe)
    img.paste(me, ME_XY, me)

    # ---- 两个血条框：对角贴边，尺寸照原版 ----
    # 对手：左上，128x32
    healthbox(f_ui, img, d, 0, 0, 128, 32,
              names["metagross"], "Lv50", 0.72)
    # 我方：右下，128x64（多 HP 数字与 EXP 条）
    healthbox(f_ui, img, d, 112, 104, 128, 48,
              names["charizard"], "Lv50", 0.44, hp_text="68/153")

    # ---- 下半屏：对话框 + 招式菜单 + 状态行 ----
    # 日志框
    dialog(d, 4, 164, 232, 52)
    f_lvgl.draw(img, 14, 168, "喷火龙 使用了 喷射火焰！", DLG_TEXT)
    f_lvgl.draw(img, 14, 189, "会心一击！造成了 187 点伤害", DLG_TEXT)

    # 招式菜单（2x2），用同一个对话框样式
    dialog(d, 4, 220, 232, 64)
    moves = [("喷射火焰", "15"), ("龙爪", "15"),
             ("翅膀攻击", "35"), ("剑舞", "20")]
    for i, (nm, pp) in enumerate(moves):
        col, row = i % 2, i // 2
        mx = 12 + col * 112
        my = 225 + row * 28
        sel = (i == 0)
        if sel:
            d.rounded_rectangle([mx, my, mx + 106, my + 24], radius=3,
                                fill=SEL_FILL, outline=SEL_EDGE, width=2)
            d.polygon([(mx + 5, my + 8), (mx + 5, my + 16), (mx + 11, my + 12)],
                      fill=SEL_TEXT)
            ink = SEL_TEXT
        else:
            d.rounded_rectangle([mx, my, mx + 106, my + 24], radius=3,
                                outline=DLG_EDGE_HI, width=1)
            ink = DLG_TEXT
        f_lvgl.draw(img, mx + 14, my + 2, nm, ink)
        pp_x = mx + 102 - f_lvgl.measure(pp)
        f_lvgl.draw(img, pp_x, my + 2, pp, DLG_GOLD if sel else DLG_TEXT)

    # 状态行（原版把这条压在对话框里，我们单独给它一个框）
    dialog(d, 4, 288, 232, 28, radius=5)
    f_lvgl.draw(img, 14, 292, "PP 15/15", DLG_GOLD)
    f_lvgl.draw(img, 100, 292, "选择招式   换人   逃跑", DLG_TEXT)

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

    f_lvgl = LVGLFont(ROOT / "gen/font_app_16.c", bpp=4)   # 对话框用
    f_ui = LVGLFont(ROOT / "gen/font_app_12.c", bpp=4)     # 血条框用（小一号）
    for nm, f in (("16px", f_lvgl), ("12px", f_ui)):
        print(f"LVGL {nm} 字体：字形 {len(f.glyphs)}，"
              f"line_height={f.line_height}")

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
        "巨金怪", "喷火龙", "Lv50", "HP", "68/153",
        "喷火龙 使用了 喷射火焰！", "会心一击！造成了 187 点伤害",
        "喷射火焰", "龙爪", "翅膀攻击", "剑舞",
        "15/15", "35/35", "20/20",
        "选择招式   换人   逃跑   返回",
    ]
    # 血条框那句用 12px 那套校验，对话框用 16px，各自独立
    box_strings = [s for s in hero_strings
                   if s in ("巨金怪", "喷火龙", "Lv50", "68/153", "HP")]
    n_box = check_charset(f_ui, box_strings, "hero.png 血条框（12px）")
    n_dlg = check_charset(f_lvgl, [s for s in hero_strings if s not in box_strings],
                          "hero.png 对话框（16px）")
    print(f"hero 文案：血条框 {n_box} 字 + 对话框 {n_dlg} 字，两套字库各自全覆盖")

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
        hero = make_hero(f_ui, f_lvgl, sprites, en2cn, args.scale)
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
