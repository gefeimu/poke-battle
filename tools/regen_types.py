"""
regen_types.py - 只重新生成 gen/poke_types.{h,c}

克制表已改为从 `data/csv/type_efficacy.csv` 派生（见 tools/type_chart.py）。
这个 CSV 只有 3 KB 且**随仓库入库**，所以本脚本不需要另外 35 个大 CSV 也能跑，
「单独重建两个文件、不惊动整条数据链」这个用途仍然成立。

用法：python tools/regen_types.py
"""

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
GEN = ROOT / "gen"

if str(ROOT / "tools") not in sys.path:
    sys.path.insert(0, str(ROOT / "tools"))
import type_chart  # noqa: E402  （必须在 sys.path 调整之后导入）

TYPES = type_chart.TYPES
N = len(TYPES)

BANNER = type_chart.BANNER

# ------------------------------------------------------------------ 回归清单
#
# 这些是历史上真出过错、或者容易被攻守方向搞反的组合。期望值**照官方规则
# 手写**，再同时与 CSV 和生成的 C 文件比对 —— 手写和 CSV 是两份独立表述，
# 不一致就说明其中一份错了，必须有人来看。
#
# 上一版的教训：这份清单当时也是照着一张错表写的（火打虫/钢写成 1x、
# 草打虫写成 2x、钢打毒写成 0x），于是错表被错断言保护着，`make test`
# 全绿而实际有 14 处与官方不符。所以现在期望值必须能追溯到官方规则本身。
#
# (攻方, [守方属性...], 期望倍率, 说明)
REGRESSION = [
    # --- 攻守方向最容易搞反的一组 ---
    ("Fire", ["Steel"], 2.0, "火打钢应克制 2x（曾误写 0.5x）"),
    ("Steel", ["Poison"], 1.0, "钢打毒是 1x（是毒打不动钢，不是钢打不动毒）"),
    ("Normal", ["Steel"], 0.5, "普通打钢不痛 0.5x"),
    ("Bug", ["Fairy"], 0.5, "虫打妖精不痛 0.5x"),
    ("Rock", ["Fighting"], 0.5, "岩石打格斗不痛 0.5x"),
    ("Psychic", ["Fairy"], 1.0, "超能打妖精是 1x"),
    # --- 复合属性相乘 ---
    # 注意毒**不抗**飞行与火：毒只抗格斗/毒/虫/草/妖精。
    # 妙蛙花是草/毒，所以吃飞行、火都是 2x * 1x = 2x，不是 1x。
    ("Flying", ["Grass", "Poison"], 2.0, "飞行打草/毒（妙蛙花）= 2x * 1x"),
    ("Fire", ["Grass", "Poison"], 2.0, "火打草/毒（妙蛙花）= 2x * 1x"),
    ("Fire", ["Bug", "Steel"], 4.0, "火打虫/钢（巨钳螳螂）4x"),
    ("Ground", ["Fire", "Steel"], 4.0, "地面打火/钢 4x"),
    ("Electric", ["Water", "Flying"], 4.0, "电打水/飞 4x"),
    ("Ice", ["Dragon", "Flying"], 4.0, "冰打龙/飞 4x"),
    ("Water", ["Grass"], 0.5, "水打草不痛 0.5x（曾误写 2x）"),
    ("Grass", ["Bug"], 0.5, "草打虫不痛 0.5x（曾误写 2x）"),
    # --- 免疫（0x） ---
    ("Electric", ["Ground"], 0.0, "电打地面 0x"),
    ("Psychic", ["Dark", "Steel"], 0.0, "超能打恶/钢：恶那边就是 0x"),
    ("Dragon", ["Fairy"], 0.0, "龙打妖精 0x"),
    ("Psychic", ["Dark"], 0.0, "超能打恶 0x"),
    ("Ghost", ["Normal"], 0.0, "幽灵打普通 0x"),
    ("Ground", ["Flying"], 0.0, "地面打飞行 0x"),
    ("Fighting", ["Ghost"], 0.0, "格斗打幽灵 0x"),
    ("Normal", ["Ghost"], 0.0, "普通打幽灵 0x"),
]


def check_regression(matrix):
    """手写期望值 vs CSV 矩阵。"""
    bad = 0
    for atk, defenders, want, why in REGRESSION:
        got = type_chart.effectiveness(matrix, atk, defenders) / 10.0
        if abs(got - want) > 1e-9:
            print(f"  [FAIL] {why}: 期望 {want}x，CSV 给 {got}x")
            bad += 1
    if bad == 0:
        print(f"  关键组合回归 {len(REGRESSION)} 条全部通过（对 CSV 校验）")
    return bad


def check_generated_matches_csv(matrix):
    """gen/poke_types.c 里的表 vs CSV。改完表忘了重跑生成器时靠这条兜住。"""
    path = GEN / "poke_types.c"
    if not path.exists():
        print("  [warn] 尚无 gen/poke_types.c，跳过")
        return 0
    src = path.read_text(encoding="utf-8")
    body = re.search(
        r"kTypeEffect\[TYPE_MATRIX_SIZE\]\[TYPE_MATRIX_SIZE\] = \{(.*?)\n\};",
        src, re.S)
    if not body:
        print("  [FAIL] 无法从 gen/poke_types.c 里解析 kTypeEffect")
        return 1
    rows = re.findall(r"\{([^{}]*)\},", body.group(1))
    if len(rows) != N:
        print(f"  [FAIL] gen/poke_types.c 的行数 {len(rows)} != {N}")
        return 1
    bad = 0
    for i, row in enumerate(rows):
        vals = [int(v) for v in row.split(",")]
        if vals != matrix[i]:
            print(f"  [FAIL] {TYPES[i]} 行与 CSV 不一致")
            print(f"         文件: {vals}")
            print(f"         CSV : {matrix[i]}")
            bad += 1
    if bad == 0:
        print("  gen/poke_types.c 的克制表与 CSV 一致（324 格全等）")
    return bad


def chinese_names():
    """18 个属性的简体中文名。

    首选从 Veekun 的 type_names.csv 转（与其它表同一条路径）。
    转不出来时退回从现有的 gen/poke_types.c 里原样搬运 —— 中文名不属于
    克制表，重建克制表不该把它弄丢（上一版就是这么丢的：regen_types.py
    根本没写 kTypeNamesCN，而 test_font.c 与一致性检查都要用它）。
    """
    from zh_names import load_names
    ids = list(range(1, N + 1))
    got = load_names("types")
    if all(tid in got and got[tid] for tid in ids):
        return [got[tid] for tid in ids]

    path = GEN / "poke_types.c"
    if path.exists():
        src = path.read_text(encoding="utf-8")
        m = re.search(r"kTypeNamesCN\[TYPE_COUNT\] = \{(.*?)\n\};", src, re.S)
        if m:
            names = re.findall(r'"((?:[^"\\]|\\.)*)"', m.group(1))
            if len(names) == N:
                print("  [note] 中文属性名沿用现有 gen/poke_types.c 里的")
                return names

    raise SystemExit(
        "取不到属性中文名：Veekun 的 type_names.csv 不可用，"
        "gen/poke_types.c 里也没有可沿用的表。")


def emit(matrix, cn_names):
    lines = [
        BANNER,
        "#ifndef POKE_TYPES_H",
        "#define POKE_TYPES_H",
        "",
        "#include <stdint.h>",
        "",
        "typedef enum {",
    ]
    for i, t in enumerate(TYPES):
        lines.append(f"    TYPE_{t.upper()} = {i},")
    lines += [
        "    TYPE_COUNT",
        "} PokeType;",
        "",
        f"#define TYPE_MATRIX_SIZE {N}",
        "",
        "/* [攻方][守方]，值 = 倍率 * 10（20=2x, 10=1x, 5=0.5x, 0=0x） */",
        "extern const uint8_t kTypeEffect[TYPE_MATRIX_SIZE][TYPE_MATRIX_SIZE];",
        "extern const char *const kTypeNames[TYPE_COUNT];",
        "",
        "/* 简体中文属性名（索引与 TYPE_* 一致），UI 显示用 */",
        "extern const char *const kTypeNamesCN[TYPE_COUNT];",
        "",
        "#endif",
        "",
    ]
    (GEN / "poke_types.h").write_text("\n".join(lines), encoding="utf-8")

    lines = [
        BANNER,
        '#include "poke_types.h"',
        "",
        "const uint8_t kTypeEffect[TYPE_MATRIX_SIZE][TYPE_MATRIX_SIZE] = {",
    ]
    for i, t in enumerate(TYPES):
        cells = ", ".join(f"{v:2d}" for v in matrix[i])
        lines.append(f"    {{ {cells} }},  /* {t} */")
    lines += ["};", "", "const char *const kTypeNames[TYPE_COUNT] = {"]
    for t in TYPES:
        lines.append(f'    "{t}",')
    lines += ["};", "", "const char *const kTypeNamesCN[TYPE_COUNT] = {"]
    for t, cn in zip(TYPES, cn_names):
        lines.append(f'    "{cn}",  /* {t} */')
    lines += ["};", ""]
    (GEN / "poke_types.c").write_text("\n".join(lines), encoding="utf-8")


def main():
    print("重新生成 gen/poke_types.{h,c}")
    matrix = type_chart.load_matrix()
    print(f"  克制表读自 {type_chart.CSV_PATH.relative_to(ROOT)}"
          f"（{type_chart.EXPECTED_ROWS} 格）")

    print("\n[回归检查]")
    bad = check_regression(matrix)
    print("\n[文件同步检查]")
    bad += check_generated_matches_csv(matrix)
    if bad:
        print(f"\n发现 {bad} 处问题，仍继续重写文件。")

    emit(matrix, chinese_names())
    print(f"\n  已写入 {GEN / 'poke_types.h'}")
    print(f"  已写入 {GEN / 'poke_types.c'}")
    print(f"  类型数={N}（Gen VI：含 Fairy）")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
