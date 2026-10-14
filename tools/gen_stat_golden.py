"""
gen_stat_golden.py - 生成 gen/poke_stat_golden.h（能力值/等级倍率的回归锚点）

内容全部来自 tools/stat_formula.py 里**手抄的、游戏外可查的**已知值，
不是从 C 实现推出来的。C 侧 test_calc.c 直接 include 这个头文件、
拿这些数去断言，于是「改坏了公式」会立刻红。

用法：
    python tools/gen_stat_golden.py
    make stat-golden
"""

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tools"))

import stat_formula as sf  # noqa: E402

GEN = ROOT / "gen"

BANNER = ("/* 自动生成：来自 tools/stat_formula.py 的外部已知值，"
          "改数据请改那里（见 tools/gen_stat_golden.py） */")

STAT_NAMES = ("hp", "atk", "def", "spa", "spd", "spe")


def render():
    """返回 gen/poke_stat_golden.h 的完整文本（不落盘，供一致性检查比对）。"""
    L = [
        BANNER,
        "#ifndef POKE_STAT_GOLDEN_H",
        "#define POKE_STAT_GOLDEN_H",
        "",
        "#include <stdint.h>",
        "",
        "/*",
        " * 能力值回归锚点。每条的 want[] 都取自游戏外可查的数值",
        " * （官方公式 + 官方种族值；烈咬陆鲨 333 速、幸福蛋 714 血这类",
        " * 是对战圈长期引用的常识值），**不是**从 src/poke_calc.c 反推的。",
        " *",
        " * species_id 用图鉴编号；ev[] 顺序 hp atk def spa spd spe。",
        " */",
        "typedef struct {",
        "    const char *note;",
        "    uint16_t species_id;",
        "    uint8_t  level;",
        "    uint8_t  nature;",
        "    uint8_t  iv;",
        "    uint16_t ev[6];",
        "    uint16_t want[6];",
        "} StatGolden;",
        "",
        f"#define STAT_GOLDEN_COUNT {len(sf.KNOWN_STATS)}",
        "static const StatGolden kStatGolden[STAT_GOLDEN_COUNT] = {",
    ]
    for e in sf.KNOWN_STATS:
        ev = ", ".join(str(x) for x in e["ev"])
        want = ", ".join(str(x) for x in e["want"])
        L.append('    { "%s", %u, %u, %u, %u, { %s }, { %s } },'
                 % (e["note"], e["species_id"], e["level"], e["nature"],
                    e["iv"], ev, want))
    L += ["};", ""]

    def table(cname, vals, note):
        L.append(f"/* {note}（下标 = stage + 6，v=100 时的结果） */")
        L.append(f"#define {cname.upper()}_COUNT {len(vals)}")
        L.append(f"static const uint16_t {cname}[{len(vals)}] = {{")
        L.append("    " + ", ".join(str(v) for v in vals) + ",")
        L.append("};")
        L.append("")

    table("kStageMul", sf.STAGE_MUL_AT_100,
          "能力值等级倍率 base=2：-1 是 66、+6 是 400")
    table("kAccStageMul", sf.ACC_MUL_AT_100,
          "命中/闪避等级倍率 base=3：-1 是 75、+1 是 133")
    L += ["#endif", ""]
    return "\n".join(L)


def emit():
    p = GEN / "poke_stat_golden.h"
    p.write_text(render(), encoding="utf-8")
    return p


if __name__ == "__main__":
    good, msgs = sf.self_check()
    if not good:
        print("stat_formula 自检未通过，拒绝生成：")
        for m in msgs:
            print(" ", m)
        raise SystemExit(1)
    p = emit()
    print(f"已生成 {p.relative_to(ROOT)}（{len(sf.KNOWN_STATS)} 组已知值）")
