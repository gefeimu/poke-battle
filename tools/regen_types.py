"""
regen_types.py - 只重新生成 gen/poke_types.{h,c}

克制表是纯硬编码的，不依赖 Veekun CSV，因此可以在不重跑整个
gen_gen3_data.py 的前提下单独重建这两个文件——避开大规模重跑
时CSV 静默截断把其他表搞坏的风险。

用法：python tools/regen_types.py
"""
import re
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
GEN = ROOT / "gen"

TYPES = [
    "Normal", "Fighting", "Flying", "Poison", "Ground", "Rock", "Bug",
    "Ghost", "Steel", "Fire", "Water", "Grass", "Electric", "Psychic",
    "Ice", "Dragon", "Dark", "Fairy",
]

# Gen VI 克制关系（攻击方 -> {守方: 倍率}），只列 != 1.0 的关系。
# 与 gen_gen3_data.py 中的 chart 保持一致（改一处必须改两处，
# 下面有assert 校验）。
CHART = {
    "Normal": {"Rock": .5, "Ghost": 0},
    "Fighting": {"Normal": 2, "Ice": 2, "Poison": .5, "Flying": .5,
                 "Psychic": .5, "Bug": .5, "Rock": 2, "Ghost": 0,
                 "Dark": 2, "Steel": 2, "Fairy": .5},
    # 飞行对草/毒都是 1.0
    "Flying": {"Fighting": 2, "Bug": 2, "Rock": .5, "Electric": .5,
               "Steel": .5},
    "Poison": {"Poison": .5, "Ground": .5, "Rock": .5, "Ghost": .5,
               "Steel": 0, "Fairy": 2},
    "Ground": {"Flying": 0, "Poison": 2, "Rock": 2, "Bug": .5, "Steel": 2,
               "Fire": 2, "Grass": .5, "Electric": 2},
    "Rock": {"Fighting": 2, "Flying": 2, "Ground": .5, "Bug": 2,
             "Steel": .5, "Fire": 2, "Ice": 2},
    "Bug": {"Fighting": .5, "Flying": .5, "Poison": .5, "Ghost": .5,
            "Dark": 2, "Steel": .5, "Fire": .5, "Grass": 2, "Psychic": 2},
    "Ghost": {"Normal": 0, "Psychic": 2, "Ghost": 2, "Dark": .5},
    "Dark": {"Fighting": .5, "Psychic": 2, "Ghost": 2, "Dark": .5,
             "Fairy": .5},
    "Steel": {"Flying": .5, "Poison": 0, "Rock": 2, "Bug": .5, "Steel": .5,
              "Fairy": 2, "Ice": 2, "Fire": .5},
    # 火打钢是 0.5x
    "Fire": {"Rock": .5, "Bug": 2, "Steel": .5, "Grass": 2, "Ice": 2,
             "Dragon": .5, "Fire": .5, "Water": .5},
    "Water": {"Ground": 2, "Rock": 2, "Fire": 2, "Water": .5, "Grass": 2,
              "Dragon": .5},
    "Grass": {"Flying": .5, "Poison": .5, "Ground": 2, "Rock": 2, "Bug": 2,
              "Steel": .5, "Fire": .5, "Water": 2, "Grass": .5,
              "Dragon": .5},
    "Electric": {"Flying": 2, "Ground": 0, "Water": 2, "Grass": .5,
                 "Electric": .5, "Dragon": .5},
    "Psychic": {"Fighting": 2, "Poison": 2, "Psychic": .5, "Dark": 0,
                "Steel": .5, "Fairy": 2},
    "Ice": {"Flying": 2, "Ground": 2, "Water": .5, "Grass": 2, "Ice": .5,
            "Dragon": 2, "Steel": .5, "Fire": .5},
    "Dragon": {"Dragon": 2, "Steel": .5, "Fairy": 0},
    "Fairy": {"Fighting": 2, "Poison": .5, "Dragon": 2, "Dark": 2,
              "Steel": .5, "Fire": .5},
}

# 关键组合回归（(攻方, 守方1, 守方2或None, 期望倍率, 说明)）。
# 这几条是历史上真出过错的地方，改表后必须仍满足。
REGRESSION = [
    ("Flying", "Grass", "Poison", 1.0, "飞行打 Venusaur 草/毒应为 1x"),
    ("Fire", "Bug", "Steel", 1.0, "火打 Scizor 虫/钢应为 1x"),
    ("Fire", "Grass", "Poison", 2.0, "火打草/毒应为 2x"),
    ("Ground", "Fire", "Steel", 4.0, "地面打火/钢应为 4x"),
    ("Electric", "Water", "Flying", 4.0, "电打水/飞应为 4x"),
    ("Ice", "Dragon", "Flying", 4.0, "冰打龙/飞�� 4x"),
    ("Electric", "Ground", None, 0.0, "电打地面应为 0x"),
    ("Psychic", "Dark", "Steel", 0.0, "钢挡超能应为 0x"),
    ("Grass", "Bug", None, 2.0, "草打虫应为 2x（曾误写 0.5x）"),
    ("Dragon", "Fairy", None, 0.0, "龙打妖精应为 0x"),
    ("Psychic", "Dark", None, 0.0, "超能打恶应为 0x"),
    ("Ghost", "Normal", None, 0.0, "幽灵打普通应为 0x"),
    ("Ground", "Flying", None, 0.0, "地面打飞行应为 0x"),
    ("Fighting", "Ghost", None, 0.0, "格斗打幽灵应为 0x"),
    ("Steel", "Poison", None, 0.0, "钢打毒应为 0x"),
    ("Normal", "Ghost", None, 0.0, "普通打幽灵应为 0x"),
]


def eff(atk: str, d1: str, d2=None) -> float:
    r = CHART.get(atk, {}).get(d1, 1.0)
    if d2 and d2 != d1:
        r *= CHART.get(atk, {}).get(d2, 1.0)
    return r


def check_regression():
    bad = 0
    for atk, d1, d2, want, why in REGRESSION:
        got = eff(atk, d1, d2)
        if abs(got - want) > 1e-9:
            print(f"  [FAIL] {why}: 实际 {got}x")
            bad += 1
    if bad == 0:
        print(f"  关键组合回归 {len(REGRESSION)} 条全部通过")
    return bad


def check_sync():
    """确保 gen_gen3_data.py 里的 chart 与本文件一致。"""
    src = (ROOT / "tools" / "gen_gen3_data.py").read_text(encoding="utf-8")
    m = re.search(r"\n    chart = \{(.*?)\n    \}", src, re.S)
    if not m:
        print("  [WARN] 未能在 gen_gen3_data.py 中定位 chart，跳过一致性检查")
        return 0
    body = m.group(1)
    # 检查每个类型行里出现的 "Target": 数字 是否与本表一致
    bad = 0
    for line in body.splitlines():
        mm = re.match(r'\s*"(\w+)":\s*\{(.*)\},?\s*$', line)
        if not mm:
            continue
        atk = mm.group(1)
        pairs = re.findall(r'"(\w+)":\s*([\d.]+)', mm.group(2))
        want = CHART.get(atk, {})
        got = {t: float(v) for t, v in pairs}
        if got != {t: float(v) for t, v in want.items()}:
            print(f"  [FAIL] {atk} 行不同步")
            print(f"         gen_gen3_data.py: {got}")
            print(f"         regen_types.py  : {dict(want)}")
            bad += 1
    if bad == 0:
        print("  gen_gen3_data.py 与 regen_types.py 的克制表一致")
    return bad


def main():
    print("重新生成 gen/poke_types.{h,c}")
    n = len(TYPES)

    lines = [
        "/* 由 tools/regen_types.py 自动生成（勿手改，改 tools/regen_types.py） */",
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
        f"#define TYPE_MATRIX_SIZE {n}",
        "",
        "/* [攻方][守方]，值 = 倍率 * 10（20=2x, 10=1x, 5=0.5x, 0=0x） */",
        f"extern const uint8_t kTypeEffect[TYPE_MATRIX_SIZE][TYPE_MATRIX_SIZE];",
        f"extern const char *const kTypeNames[TYPE_COUNT];",
        "",
        "#endif",
        "",
    ]
    (GEN / "poke_types.h").write_text("\n".join(lines), encoding="utf-8")

    lines = [
        "/* 由 tools/regen_types.py 自动生成（勿手改，改 tools/regen_types.py） */",
        '#include "poke_types.h"',
        "",
        f"const uint8_t kTypeEffect[TYPE_MATRIX_SIZE][TYPE_MATRIX_SIZE] = {{",
    ]
    for atk in TYPES:
        row = [10] * n
        for target, mult in CHART.get(atk, {}).items():
            if target in TYPES:
                row[TYPES.index(target)] = int(round(mult * 10))
        cells = ", ".join(f"{v:2d}" for v in row)
        lines.append(f"    {{ {cells} }},  /* {atk} */")
    lines += [
        "};",
        "",
        "const char *const kTypeNames[TYPE_COUNT] = {",
    ]
    for t in TYPES:
        lines.append(f'    "{t}",')
    lines += ["};", ""]
    (GEN / "poke_types.c").write_text("\n".join(lines), encoding="utf-8")
    print(f"  已写入 {GEN / 'poke_types.h'}")
    print(f"  已写入 {GEN / 'poke_types.c'}")
    print(f"  类型数={n}（Gen VI：含 Fairy）")


if __name__ == "__main__":
    print("\n[同步检查]")
    bad = check_sync()
    print("\n[回归检查]")
    bad += check_regression()
    if bad:
        print(f"\n发现 {bad} 处问题，仍继续重写文件。")
    print()
    main()