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
  [15] 属性克制表与官方 CSV 全量比对

用法：python tools/check_consistency.py
"""
import re
import subprocess
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



# ---- 各组测试的二进制名单（顺序即 README 表格的顺序）----
TEST_NAMES = [
    "data_selfcheck", "test_calc", "test_battle", "test_ai", "test_sprites",
    "test_font", "test_render", "test_input", "test_ui", "test_session",
]


def find_test_binary(name):
    """定位 build/<name> 的可执行文件，找不到返回 None。

    **不要写成 ROOT/"build"/name 就完事**：本机是 MSYS/Git Bash 下的 MinGW
    工具链，链接产出的名字带 `.exe`（build/test_ui.exe），不带扩展名的那个
    文件**根本不存在**。曾经按无扩展名判断，于是十个测试全部 exists()==False，
    整组 [9] 悄悄降级成十行 [warn] 还照样 exit 0 —— 一个「只做提醒、从不拦截」
    的校验，比没有校验更糟，因为它看起来是绿的。

    所以这里按「无扩展名 → .exe → 目录里任何 <name>.*」的顺序找，并且
    调用方把「找不到」当**失败**处理。
    """
    build = ROOT / "build"
    for cand in (build / name, build / f"{name}.exe"):
        if cand.is_file():
            return cand
    # 兜底：某些工具链会加别的可执行后缀。
    # **必须限定后缀白名单**：曾经写成「任意 build/<name>.*」，结果把手工
    # 备份出来的 test_ai.exe.bak 也当成了测试本体 —— Windows 的 CreateProcess
    # 不看扩展名，只要是合法 PE 就照跑，于是「删掉可执行文件」的验证照样全绿。
    # 一个会把备份当产物的查找函数，等于没有查找。
    for suf in (".bin", ".out", ".elf"):
        p = build / f"{name}{suf}"
        if p.is_file():
            return p
    return None


def run_test_count(name):
    """跑一遍 build/<name>，返回通过数；跑不成/有失败项返回 None。

    各测试的汇总行格式不完全统一（有的带「结果:」有的不带），
    所以只认 `N 通过, M 失败` 这一段。
    """
    exe = find_test_binary(name)
    if exe is None:
        return None
    try:
        r = subprocess.run([str(exe)], cwd=str(ROOT), capture_output=True,
                           text=True, encoding="utf-8", errors="replace",
                           timeout=600)
    except (OSError, subprocess.SubprocessError):
        return None
    m = re.search(r"(\d+) 通过,\s*(\d+) 失败", r.stdout)
    if not m or int(m.group(2)) != 0:
        return None
    return m.group(1)


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

    # ---- 9. README 测试计数（以实际跑出来的数字为准）----
    #
    # 这张清单原先是在这里手写的，于是和 README 各抄一份、必然会漂 ——
    # 实际就漂过：test_calc 这里写 83 而真实是 84，README 另一处表里
    # test_ui 写 94 而真实是 118。所以改成**跑一遍二进制、读它的汇总行**，
    # README 里记的数字必须等于实测值。这和克制表那次是同一个教训：
    # 能被实测的东西就不要手抄。
    print("\n[9] README 测试计数 = 实测")
    measured = []
    for name in TEST_NAMES:
        # 「找不到二进制」必须是 FAIL 而不是 warn：make test 里本组排在
        # 各测试目标之后，跑不成的唯一原因就是构建/链接出了问题，这种时候
        # 静默跳过会把「测试根本没编出来」伪装成「一切正常」。
        if find_test_binary(name) is None:
            check(False, f"build/{name} 不存在（先 make test 再跑本检查？）")
            continue
        n = run_test_count(name)
        if n is None:
            check(False, f"{name} 没能跑通（编译过但断言失败/摘要行缺失）")
            continue
        measured.append((name, int(n)))

    for name, n in measured:
        row = re.search(rf"^\|\s*`{name}`\s*\|([^|]*)\|([^|]*)\|\s*(\d+)\s*\|",
                        rd, re.M)
        check(bool(row) and int(row.group(3)) == n,
              f"README 表格里 {name} = {row.group(3) if row else '缺行'}"
              f"（实测 {n}）")
        check(f"{n} 项" in rd, f"README 正文提到 {name} 的 {n} 项")

    check(len(measured) == len(TEST_NAMES),
          f"{len(measured)}/{len(TEST_NAMES)} 个测试实测成功"
          "（缺的那个说明构建或断言有问题，不能当通过）")
    if len(measured) == len(TEST_NAMES):
        total = sum(n for _, n in measured)
        check(f"{total} 项断言" in rd, f"README 记录总计 {total} 项（实测合计）")
        check(f"{total} 项断言全过" in rd or f"**{total} 项断言" in rd,
              f"README 顶部写了 {total} 项断言全过")
    else:
        print(f"  [note] 只有 {len(measured)}/{len(TEST_NAMES)} 个测试跑成，"
              "总计无法定论 —— 上面那条 FAIL 才是要看的东西")

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

    # ---- 15. 属性克制表与官方 CSV ----
    #
    # 这张表是**手写 + 自证式断言**这套做法的反面教材：它在两个生成器里各手写
    # 了一份，而 regen_types.py 的「回归断言」又是照着手写表写的，于是错表和
    # 错断言互相印证，`make test` 一直全绿，实际有 14 处与官方规则不符
    # （最严重的是「火打钢」写成了 0.5x，本该是 2x）。
    #
    # 现在数据只认 data/csv/type_efficacy.csv，这里做 324 格全量比对；
    # 另外把 22 条关键组合的**官方期望值**再独立写一遍对 CSV 校验 ——
    # 万一 CSV 本身被改坏，这一层也能兜住。
    print("\n[15] 属性克制表 = 官方 CSV（324 格）")
    sys.path.insert(0, str(ROOT / "tools"))
    import regen_types
    import type_chart

    matrix = type_chart.load_matrix()
    check(len(matrix) == 18 and all(len(r) == 18 for r in matrix),
          "CSV 给出 18x18 矩阵")
    check(type_chart.EXPECTED_ROWS == 324, "CSV 组合数 = 324")

    tc = read("gen/poke_types.c")
    body = re.search(
        r"kTypeEffect\[TYPE_MATRIX_SIZE\]\[TYPE_MATRIX_SIZE\] = \{(.*?)\n\};",
        tc, re.S)
    check(bool(body), "能解析 gen/poke_types.c 的 kTypeEffect")
    if body:
        rows = [[int(v) for v in r.split(",")]
                for r in re.findall(r"\{([^{}]*)\},", body.group(1))]
        check(len(rows) == 18, f"生成文件 18 行（实际 {len(rows)}）")
        check(rows == matrix, "324 格与 data/csv/type_efficacy.csv 全等")
        if rows != matrix:
            for i, (a, b) in enumerate(zip(rows, matrix)):
                if a != b:
                    print(f"         {type_chart.TYPES[i]} 行: 文件 {a}")
                    print(f"         {type_chart.TYPES[i]} 行: CSV  {b}")

    # 官方期望值（与生成器里的清单同一份，不另抄一遍）
    bad = 0
    for atk, defenders, want, why in regen_types.REGRESSION:
        got = type_chart.effectiveness(matrix, atk, defenders) / 10.0
        if abs(got - want) > 1e-9:
            bad += 1
            print(f"  [FAIL] {why}: CSV 给 {got}x，官方 {want}x")
    check(bad == 0,
          f"关键组合 {len(regen_types.REGRESSION)} 条与官方规则一致")

    for token in ("type_efficacy.csv", "type_chart.py"):
        check(token in rd, f"README 记录了克制表来源 {token}")
    check("type_efficacy" in read("tools/gen_gen3_data.py"),
          "主生成器注明克制表来自 type_efficacy.csv")
    check("chart = {" not in read("tools/gen_gen3_data.py")
          and "CHART = {" not in read("tools/regen_types.py"),
          "两个生成器里都不再有手写的克制表")

    # ---- 16. 菜单版式与界面集合 ----
    print("\n[16] 菜单版式与界面集合")
    uh = read("src/poke_ui.h")
    us = read("src/poke_ui_state.c")
    ds = read("test/dump_screen.c")

    # 16.1 一屏最多几行，必须装得下状态机里最大的那个界面。
    # 状态机按 ui_screen_rows() 算滚动窗口，绘制按 UI_MENU_ROWS_MAX 留高度；
    # 哪天状态机把某个界面加到 9 行，绘制就会把第 9 行画到状态行上 ——
    # 光标移到了看得见、内容画不出来，这种 bug 只能靠两边对数抓。
    m = re.search(r"#define\s+UI_MENU_ROWS_MAX\s+(\d+)", uh)
    rows_max = int(m.group(1)) if m else 0
    rows_map = dict(re.findall(
        r"case\s+(UI_SCREEN_\w+):\s*return\s+(\d+);", us))
    # ui_screen_rows 的 default 分支兜底成 1，不在 map 里；有枚举名却没有
    # return 的才算漏
    screens_in_fn = set(re.findall(r"case\s+(UI_SCREEN_\w+):", us))
    missing = sorted(screens_in_fn - set(rows_map))
    check(bool(rows_map) and not missing,
          f"ui_screen_rows 的每个界面都有返回值（缺 {missing or '无'}）")
    if rows_map:
        want_max = max(int(v) for v in rows_map.values())
        check(rows_max >= want_max,
              f"UI_MENU_ROWS_MAX = {rows_max} 装得下最大界面 {want_max} 行")

    # 16.2 dump_screen 的屏清单必须覆盖状态机的每个界面。
    # 新加一个 UiScreen 而忘了导出/验收它，这里会点名。
    m = re.search(r"static const char \*const kScreens\[\] = \{(.*?)\};",
                  ds, re.S)
    listed = set(re.findall(r'"(\w+)"', m.group(1))) if m else set()
    # log 是会话的**阶段**不是状态机界面，映射成战斗界面
    screens = {s.replace("UI_SCREEN_", "").lower() for s in screens_in_fn}
    check(m is not None, "dump_screen 里有 kScreens 清单")
    check(screens <= listed,
          f"dump_screen 覆盖每个界面（缺 {sorted(screens - listed) or '无'}）")
    check(listed - screens == {"log"},
          f"清单里只有 log 不是状态机界面（多出 {sorted(listed - screens)}）")

    # 16.3 会话层的分层必须覆盖弹层类界面。弹层是「盖在战斗界面上」的，
    # 忘了在 ses_layer 里把某个界面归到 BATTLE_MENU，它就会被当成整屏菜单
    # 画出来 —— 界面还能用，只是背后那块战斗画面凭空消失。
    ses_c = read("src/poke_session.c")
    for sc in ("UI_SCREEN_ACTION", "UI_SCREEN_CONFIRM"):
        check(sc in ses_c, f"ses_layer 认识 {sc}")

    # ============================================================
    # 17. 能力值公式 / 等级倍率 / 性格 = 外部已知值
    #
    # 本项目已经因为「手写数据 + 无上游校验 + 测试照着实现写期望值」
    # 栽过四次（克制表 14 格、能力值公式、等级倍率表 11 档、性格 4+24 处），
    # 而且每一次都是 `make test` 全绿。这一组把能力相关的数值模型钉死：
    # 唯一权威是 tools/stat_formula.py，C 侧只能引用它生成的头文件。
    # ============================================================
    print("\n[17] 能力值公式 / 等级倍率 / 性格 = 外部已知值")
    if str(ROOT / "tools") not in sys.path:
        sys.path.insert(0, str(ROOT / "tools"))
    import importlib
    sf = importlib.import_module("stat_formula")
    gsg = importlib.import_module("gen_stat_golden")

    # 17.0 参考实现自己必须能重算出那些**游戏外可查**的已知值
    sf_ok, sf_msgs = sf.self_check()
    check(sf_ok, "stat_formula 参考实现能重算全部外部已知值"
          + ("" if sf_ok else "（" + "；".join(sf_msgs[:3]) + "）"))

    # 17.1 生成的头文件必须与生成器输出逐字节一致 —— 改了已知值却忘了
    # 重跑 make stat-golden 时，测试还在拿旧期望值断言。
    golden = read("gen/poke_stat_golden.h")
    check(bool(golden) and golden == gsg.render(),
          "gen/poke_stat_golden.h 与生成器输出一致（改过请重跑 make stat-golden）")

    m = re.search(r"static const uint16_t kStageMul\[\d+\] = \{(.*?)\};",
                  golden, re.S)
    vals = [int(v) for v in re.findall(r"\d+", m.group(1))] if m else []
    check(vals == list(sf.STAGE_MUL_AT_100), "golden 的能力值等级表 = 官方倍率")
    check(len(vals) == 13 and vals[0] < vals[6] < vals[12],
          "等级表覆盖 -6..+6 且单调递增（负档真的降、正档真的升）")

    # 17.2 C 侧不能再有手写的等级查表，且公式必须是官方那一条
    calc_c = read("src/poke_calc.c")
    # 认「声明」而不是「出现过」——注释里提到旧表名是允许的（正是在解释它为什么被删）
    check(not re.search(r"const\s+int8_t\s+NUM\s*\[\s*13\s*\]", calc_c)
          and not re.search(r"const\s+int8_t\s+DEN\s*\[\s*13\s*\]", calc_c),
          "poke_calc.c 里不再有手写的 NUM[13]/DEN[13] 等级表")
    check(calc_c.count("2 * base + iv + ev / 4") == 2,
          "calc_hp/calc_stat 各用了一处 (2*Base + IV + EV/4)（IV 不砍半、EV 在等级缩放之内）")
    check("calc_stage_ratio" in calc_c
          and "calc_stage_ratio" in read("src/poke_battle.c"),
          "能力值与命中/闪避等级共用 calc_stage_ratio（不再各写一套）")

    # 17.3 已知值里手抄的种族值必须与生成的种族表一致
    #      （曾经把巨金怪 HP 写成 70，实际 80）
    species_rows = {}
    for line in read("gen/poke_species.c").splitlines():
        s = line.strip()
        if not s.startswith("{") or '"' not in s:
            continue
        body = s.rstrip(",").strip()
        if body.endswith("}"):
            body = body[:-1]
        parts = [p.strip() for p in body[1:].split(",")]
        if len(parts) < 14:
            continue
        try:
            pid = int(parts[0])
            hp6 = tuple(int(x) for x in parts[8:14])
        except ValueError:
            continue
        species_rows[pid] = hp6
    bad = []
    for e in sf.KNOWN_STATS:
        got = species_rows.get(e["species_id"])
        if got != tuple(e["base"]):
            bad.append(f"#{e['species_id']} 表里 {got} ≠ 已知值 {tuple(e['base'])}")
    check(bool(species_rows) and not bad,
          "已知值里的种族值与 gen/poke_species.c 一致"
          + ("（" + "；".join(bad) + "）" if bad else ""))

    # 17.4 性格表：id / 英文名 / (升,降) 必须与 stat_formula 一致
    nature_rows = {}
    for line in read("gen/poke_natures.c").splitlines():
        mm = re.search(r'\{\s*(\d+),\s*(\d+),\s*"(\w+)",\s*"([^"]*)"\s*\},'
                       r'\s*/\* nature (\d+) \*/', line)
        if mm:
            nature_rows[int(mm.group(5))] = (
                mm.group(3), int(mm.group(1)), int(mm.group(2)), mm.group(4))
    bad = []
    for nid, name in sf.NATURE_ORDER.items():
        got = nature_rows.get(nid)
        inc, dec = sf.NATURE_EFFECT[name]
        if not got or got[0] != name or got[1] != inc or got[2] != dec:
            bad.append(f"nature {nid}({name})")
    check(bool(nature_rows) and not bad,
          "gen/poke_natures.c 的性格 id/英文名/(+,-) 与 stat_formula 一致"
          + ("（" + "；".join(bad) + "）" if bad else ""))
    no_cn = sorted(nid for nid, v in nature_rows.items() if not v[3])
    check(not no_cn, f"每个性格都有中文名（缺 {no_cn or '无'}）")

    # 17.5 测试必须真的引用 golden，而不是又抄一份期望值
    check("poke_stat_golden.h" in read("test/test_calc.c"),
          "test_calc.c 引用 golden 头文件（期望值来自外部已知值）")
    rd2 = read("README.md")
    for token in ("tools/stat_formula.py", "poke_stat_golden.h"):
        check(token in rd2, f"README 提到了 {token}")

    print(f"\n=== 结果: {g_fail} 处不一致 ===")
    return 0 if g_fail == 0 else 1


if __name__ == "__main__":
    sys.exit(main())