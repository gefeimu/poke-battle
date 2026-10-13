"""
check_consistency.py - 跨文件/跨实现的交叉一致性检查

为什么需要这个脚本
------------------
本项目精灵图的约定曾经散落在 6 处：README 顶部汇总、README 数据规格表、
README 详细段落、build_sprites_c.py 的默认值、gen/poke_sprites.h 的常量、
gen/poke_sprites.c 的文件头注释。这 6 处互相矛盾过，而且**没人发现
图本身是坏的**（4bpp 被误降成 2bpp）。

文档对不上顶多是误导，但「数据坏了而文档也跟着错」就没人能自查了。
所以把「哪些值必须一致」写成断言，让 `make test` 之外的独立入口也能跑。

后来又长出两组「同一份数值出现在两处」的检查：
  [12] 固件渲染层（C）与 PC 预览（Python）的版式常量
  [13] 生成的字库数据与 lv_font_conv 源文件的度量
  [14] 输入层的电压窗口/时序与官方 BSP 的数值

用法：python tools/check_consistency.py
"""
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent

# ---- 精灵图的权威值（改这里之前先想清楚，要同步 README 与生成脚本）----
AUTHORITATIVE = {
    "sprite_count": 427,   # 正面 386 + Mega 41
    "sprite_size": 64,     # 画布边长
    "palette_size": 16,    # RGB565 调色板槽位
    "front_count": 386,
    "mega_count": 41,
    "bytes_per_sprite": 64 * 64 // 2,  # 4bpp = 每字节 2 像素
    "font_cell": 14,                   # 字格边长（宽=高）
    "front_source": "firered-leafgreen",
    "mega_source": "black-white",
    "normal_source_dir": "generation-iii",
    "mega_source_dir": "generation-v",
}

g_fail = 0


def check(cond, msg):
    global g_fail
    if cond:
        print(f"  [ OK ] {msg}")
    else:
        g_fail += 1
        print(f"  [FAIL] {msg}")


def read(rel):
    p = ROOT / rel
    return p.read_text(encoding="utf-8") if p.exists() else ""


def main():
    print("=== 精灵图一致性交叉检查 ===\n")
    A = AUTHORITATIVE

    rd = read("README.md")
    bs = read("tools/build_sprites_c.py")
    sh = read("gen/poke_sprites.h")
    ts = read("test/test_sprites.c")
    ds = read("test/dump_sprites.c")
    mk = read("Makefile")
    sc_head = "\n".join(read("gen/poke_sprites.c").split("\n")[:20])

    # ---- 1. 头文件常量 ----
    print("[1] gen/poke_sprites.h 常量")
    for name, want in [("SPRITE_COUNT", A["sprite_count"]),
                       ("SPRITE_SIZE", A["sprite_size"]),
                       ("PALETTE_SIZE", A["palette_size"])]:
        m = re.search(rf"#define\s+{name}\s+(\d+)", sh)
        check(bool(m) and int(m.group(1)) == want,
              f"{name} = {m.group(1) if m else '缺失'}（应为 {want}）")

    # ---- 2. 生成脚本常量 ----
    print("\n[2] tools/build_sprites_c.py 常量")
    m = re.search(r"ICON_SIZE\s*=\s*(\d+)", bs)
    check(bool(m) and int(m.group(1)) == A["sprite_size"],
          f"ICON_SIZE = {m.group(1) if m else '缺失'}（应为 {A['sprite_size']}）")
    m = re.search(r"PALETTE_COLORS\s*=\s*(\d+)", bs)
    check(bool(m) and int(m.group(1)) == A["palette_size"],
          f"PALETTE_COLORS = {m.group(1) if m else '缺失'}（应为 {A['palette_size']}）")
    m = re.search(r'--gen3",\s*default="(\w[\w-]*)"', bs)
    check(bool(m) and m.group(1) == A["front_source"],
          f"--gen3 默认 = {m.group(1) if m else '缺失'}（应为 {A['front_source']}）")

    # ---- 3. 位深：生成/消费/文档三处必须都是 4bpp ----
    print("\n[3] 位深三处一致（4bpp）")
    check("(v & 0x0F) << (4 - k * 4)" in bs, "生成端按 4bpp 打包")
    check("(v & 0x03) << (6 - k * 2)" not in bs.split("注意这里曾是")[0],
          "生成端无残留的 2bpp 打包")
    check(re.search(r"\(i\s*%\s*2\)\s*\?\s*0\s*:\s*4", ts) is not None,
          "test_sprites.c 按 4bpp 解码")
    check(re.search(r"\(i4\s*%\s*2\)\s*\?\s*0\s*:\s*4", ds) is not None,
          "dump_sprites.c 按 4bpp 解码")
    for name, txt in [("README", rd), ("build_sprites_c.py", bs)]:
        # 「每字节 4 像素」只允许出现在解释旧 bug 的历史说明里
        bad = []
        for pat in ("每字节 4 像素，2 bit", "4 像素高位在前"):
            idx = txt.find(pat)
            while idx != -1:
                ctx = txt[max(0, idx - 100):idx]
                if "注意" not in ctx and "曾是" not in ctx:
                    bad.append(pat)
                idx = txt.find(pat, idx + 1)
        check(not bad, f"{name} 无过时的位深描述")

    # ---- 4. 每图字节数 ----
    print("\n[4] 每图字节数")
    check(A["bytes_per_sprite"] == 2048,
          f"64*64/2 = {A['bytes_per_sprite']} 字节")
    check("/2 字节" in sh or f"/{A['sprite_size']}*{A['sprite_size']}/2" in bs,
          "头文件/脚本标注的字节数用 /2（4bpp）")

    # ---- 5. 图源 ----
    print("\n[5] 图源")
    check(A["normal_source_dir"] in bs, f"正面源目录 {A['normal_source_dir']}")
    check(A["mega_source_dir"] in bs, f"Mega 源目录 {A['mega_source_dir']}")
    check(A["front_source"] in rd, "README 记录了当前正面图源")
    check(A["front_source"] in sc_head, "C 文件头注释记录了当前正面图源")
    # 不该再有「三代正面/背面」这类描述（只要正面）
    check("三代正面/背面" not in rd, "README 无「三代正面/背面」表述")

    # ---- 6. 只要正面 ----
    print("\n[6] 导入范围（只要正面）")
    check("keep_back=False" in bs, "脚本默认不导背面")
    check('"--back"' in bs, "有 --back 开关")
    check("背面/闪亮" in ts, "test_sprites.c 断言不含背面/闪亮")

    # ---- 7. 生成文件自述与实际一致 ----
    print("\n[7] gen/poke_sprites.c 文件头注释")
    check("4 bit/像素" in sc_head, "文件头标注 4 bit/像素")
    check("每字节 2 像素" in sc_head, "文件头标注每字节 2 像素")
    m = re.search(r"构成\s+(\d+)\s*KB\s*正面", sc_head)
    want_front_kb = A["front_count"] * A["bytes_per_sprite"] // 1024
    check(bool(m) and int(m.group(1)) == want_front_kb,
          f"文件头正面 {m.group(1) if m else '?'} KB（应为 {want_front_kb} KB）")
    m = re.search(r"合计\s+(\d+)\s*KB", sc_head)
    want_total_kb = A["sprite_count"] * A["bytes_per_sprite"] // 1024
    check(bool(m) and int(m.group(1)) == want_total_kb,
          f"文件头合计 {m.group(1) if m else '?'} KB（应为 {want_total_kb} KB）")

    # ---- 8. 构建入口 ----
    print("\n[8] Makefile 入口")
    check("test_sprites" in mk, "test_sprites 在 make test 里")
    check("sprites-preview" in mk, "有 sprites-preview 目标")
    check("dump_sprites" in mk, "sprites-preview 依赖 dump_sprites")

    # ---- 9. README 测试计数 ----
    print("\n[9] README 测试计数")
    counts = [("data_selfcheck", 83), ("test_calc", 83), ("test_battle", 100),
              ("test_ai", 19), ("test_sprites", 14), ("test_font", 25),
              ("test_render", 79), ("test_input", 51), ("test_ui", 118)]
    for name, n in counts:
        check(f"{n} 项" in rd, f"README 记录 {name} = {n} 项")

    total = sum(n for _, n in counts)
    check(f"{total} 项断言" in rd, f"README 记录总计 {total} 项")

    # ---- 10. 字库规格一致 ----
    print("\n[10] 中文字库规格")
    fh = read("gen/font_cn.h")
    gf = read("tools/gen_font.py")

    m = re.search(r"#define FONT_CELL_W\s+(\d+)", fh)
    fw = int(m.group(1)) if m else 0
    m = re.search(r"#define FONT_CELL_H\s+(\d+)", fh)
    fhc = int(m.group(1)) if m else 0
    m = re.search(r"#define FONT_STRIDE\s+(\d+)", fh)
    fstride = int(m.group(1)) if m else 0
    m = re.search(r"CELL_W,\s*CELL_H\s*=\s*(\d+),\s*(\d+)", gf)
    gw, gh = (int(m.group(1)), int(m.group(2))) if m else (0, 0)

    check(fw > 0 and fw == gw and fhc == gh,
          f"字格生成端与头文件一致（脚本 {gw}x{gh}，头文件 {fw}x{fhc}）")
    check(fstride == (fw + 7) // 8,
          f"FONT_STRIDE = {fstride} 等于 ceil({fw}/8) = {(fw + 7) // 8}")
    # 生成端不能残留整数除法
    check("FONT_STRIDE = (CELL_W + 7) // 8" in gf,
          "生成端用向上取整定义 FONT_STRIDE")
    check("range(CELL_W // 8)" not in gf,
          "生成端无 `CELL_W // 8` 的整数除法（会丢像素）")

    m = re.search(r"#define FONT_MAP_COUNT\s+(\d+)", fh)
    fcount = int(m.group(1)) if m else 0
    check(fcount > 1000, f"字库收录 {fcount} 字（应 >1000）")
    check(str(fcount) in rd, f"README 记录了收录字数 {fcount}")

    # 生成端必须同时扫 gen 与 src
    check('ROOT / "src"' in gf, "字符收集同时扫 src 目录（日志文案在那里）")
    # 字号默认跟字格
    check("default=CELL_H" in gf, "字号默认值跟随字格高度（避免裁笔画）")

    # ---- 11. 中文名字段 ----
    print("\n[11] 数据表中文字段")
    for hf, struct in [("gen/poke_species.h", "SpeciesDef"),
                       ("gen/poke_moves.h", "MoveDef"),
                       ("gen/poke_abilities.h", "AbilityDef"),
                       ("gen/poke_items.h", "ItemDef"),
                       ("gen/poke_natures.h", "NatureDef")]:
        txt = read(hf)
        check("name_cn" in txt, f"{hf} 有 name_cn 字段")
    check("kTypeNamesCN" in read("gen/poke_types.h"), "poke_types.h 有中文属性名")
    # 生成器要带中文名
    gg = read("tools/gen_gen3_data.py")
    check("zh_names" in gg, "主生成器导入 zh_names")
    check("load_names" in gg, "主生成器调用 load_names")

    # ---- 12. 渲染层版式与 PC 预览一致 ----
    #
    # 界面现在有**两套独立实现**：固件侧 src/poke_ui.c，PC 侧
    # tools/make_readme_images.py（README 配图）。它们必须用同一份版式
    # 数值，否则「预览图好看、固件里错位」这种问题永远发现不了 ——
    # 而且 make render-compare 的比对会直接失败。
    #
    # 实际已经因此踩过一次：状态行的文字坐标一边写成相对偏移、一边是绝对
    # 坐标，差 4px；招式选中格的 bbox 一边含端点一边不含，差 1px。
    print("\n[12] 渲染层版式与 PC 预览一致")
    # 屏幕尺寸在 .h 里，其余版式常量在 .c 里
    ui = read("src/poke_ui.c") + read("src/poke_ui.h")
    mk = read("tools/make_readme_images.py")

    def macro(text, name):
        m = re.search(rf"#define\s+{name}\s+(-?\d+)", text)
        return int(m.group(1)) if m else None

    def pyvar(text, name):
        """
        取 Python 模块级整数常量的值。

        要支持 `A, B = 240, 320` 这种一行赋多值的写法（屏幕尺寸就是这么写的）。
        注意必须**遍历所有匹配**去找包含目标名的那一条 —— 用 search 只会拿到
        文件里第一个 `NAME = 数字`，匹配不到多值行，然后静默返回 None。
        """
        pat = (r"^\s*([A-Z_]+(?:\s*,\s*[A-Z_]+)*)\s*=\s*"
               r"(-?\d+(?:\s*,\s*-?\d+)*)")
        for m in re.finditer(pat, text, re.M):
            names = [x.strip() for x in m.group(1).split(",")]
            vals = [int(x.strip()) for x in m.group(2).split(",")]
            if name in names and len(names) == len(vals):
                return vals[names.index(name)]
        return None

    pairs = [("UI_SCREEN_W", "SCREEN_W"), ("UI_SCREEN_H", "SCREEN_H"),
             ("HB_INSET", "HP_INSET"), ("HP_ROW_H", "HP_ROW_H")]
    for c_name, py_name in pairs:
        cv, pv = macro(ui, c_name), pyvar(mk, py_name)
        check(cv is not None and cv == pv,
              f"{c_name}(C) = {cv} 与 {py_name}(PC) = {pv} 一致")

    for c_name, val in [("HB_FOE_W", 128), ("HB_FOE_H", 32),
                        ("HB_ME_W", 128), ("HB_ME_H", 44)]:
        check(macro(ui, c_name) == val, f"{c_name} = {val}")
        check(str(val) in mk, f"PC 预览里也出现 {val}（血条框尺寸）")

    # ---- 13. 字体数据与源 .c 的度量一致 ----
    #
    # gen/poke_font_data.c 是从 lv_font_conv 的输出转来的。转换器如果读错
    # line_height / base_line，字形会整体上下偏移 —— 而两份文件各自都
    # 「自洽」，只有对起来才能发现。
    print("\n[13] 字体数据与 LVGL 源文件一致")
    fd = read("gen/poke_font_data.c")
    for lvgl_file, ident in [("gen/font_app_16.c", "kFontApp16"),
                             ("gen/font_app_12.c", "kFontApp12"),
                             ("gen/font_hp_10.c", "kFontHp10")]:
        src = read(lvgl_file)
        m_lh = re.search(r"\.line_height\s*=\s*(\d+)", src)
        m_bl = re.search(r"\.base_line\s*=\s*(-?\d+)", src)
        m_gl = re.search(r"\.bpp\s*=\s*(\d+)", src)
        if not (m_lh and m_bl and m_gl):
            check(False, f"{lvgl_file} 里找不到度量")
            continue
        lh, bl, bpp = int(m_lh.group(1)), int(m_bl.group(1)), int(m_gl.group(1))
        # 生成的结构体形如： "font_app_16", 4, 21, 5,
        m = re.search(rf'"{Path(lvgl_file).stem}",\s*(\d+),\s*(\d+),\s*(-?\d+)',
                      fd)
        if not m:
            check(False, f"{ident} 在 gen/poke_font_data.c 里找不到")
            continue
        g_bpp, g_lh, g_bl = int(m.group(1)), int(m.group(2)), int(m.group(3))
        check(g_bpp == bpp and g_lh == lh and g_bl == bl,
              f"{ident} 的 bpp/line_height/base_line = "
              f"{g_bpp}/{g_lh}/{g_bl}，源文件是 {bpp}/{lh}/{bl}")

    # 三套字体都得在数据文件里
    for ident in ("kFontApp16", "kFontApp12", "kFontHp10"):
        check(f"const BitmapFont {ident} =" in fd,
              f"gen/poke_font_data.c 导出了 {ident}")

    # ---- 14. 输入层的电压窗口/时序与官方 BSP 一致 ----
    #
    # 这一层的数值全部抄自官方仓，而且**每一个抄错都不报错**：窗口边界差
    # 1 mV 就是按键偶尔判成另一个键；阈值少写一个 0 就是长按永远不触发。
    # 所以把「哪几个数必须等于多少」写成断言，顺带钉住「实现里不许出现
    # 裸数字」—— 数字一旦从 .h 抄进 .c，就会和 .h 悄悄漂移。
    print("\n[14] 输入层常量与官方 BSP 一致")
    pin = read("src/poke_input.h")
    pim = read("src/poke_input.c")
    sth = read("src/poke_ui_state.h")
    stc = read("src/poke_ui_state.c")

    # 预期值来自 components/bsp/include/bsp_pins.h 与官方 fork 的键位锁：
    #   BSP_BTN_MV_TABLE = { {0,150}, {150,447}, {447,1900} }
    #   BSP_BTN_LONG_PRESS_MS = 500
    #   键位锁 40 / 120 / 120
    for name, want in [("KEY_MV_UP_MAX", 150), ("KEY_MV_DOWN_MAX", 447),
                       ("KEY_MV_OK_MAX", 1900), ("KEY_PRESS_CONFIRM_MS", 40),
                       ("KEY_LOCK_SWITCH_MS", 120), ("KEY_LONG_PRESS_MS", 500)]:
        m = re.search(rf"#define\s+{name}\s+(\d+)", pin)
        check(bool(m) and int(m.group(1)) == want,
              f"{name} = {m.group(1) if m else '缺失'}（官方值 {want}）")
        check(name in pim, f"poke_input.c 用 {name} 判定，没有裸数字")

    # 连发节奏属于应用自己的 tick（官方文档明确说不归驱动管）
    for name, want in [("UI_REPEAT_DELAY_MS", 400), ("UI_REPEAT_RATE_MS", 120)]:
        m = re.search(rf"#define\s+{name}\s+(\d+)", sth)
        check(bool(m) and int(m.group(1)) == want,
              f"{name} = {m.group(1) if m else '缺失'}")
        check(name in stc, f"poke_ui_state.c 用 {name}，没有裸数字")

    # 出处要写下来 —— 这几个数是查证来的，不是估的
    for token in ("three-key-input-event-semantics",
                  "adc-ladder-keypad-long-press-misread", "bsp_pins.h"):
        check(token in pin, f"poke_input.h 注明了出处 {token}")
    for token in ("150", "447", "1900", "500 ms", "40 ms", "120 ms", "2959"):
        check(token in rd, f"README 记录了输入层依据 {token}")
    check("键位锁" in rd and "键位锁" in pin,
          "README 与源码都写了「键位锁」这一层")
    check("读失败" in rd and "读失败" in pin,
          "README 与源码都写了「ADC 读失败当未按下」")

    print(f"\n=== 结果: {g_fail} 处不一致 ===")
    return 0 if g_fail == 0 else 1


if __name__ == "__main__":
    sys.exit(main())