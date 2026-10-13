"""
type_chart.py - 属性克制表的**唯一**数据源

从 `data/csv/type_efficacy.csv`（Veekun / PokeAPI，与 data/csv 其余表同源）
读出 18x18 克制表。C 侧只由生成器写入 `gen/poke_types.c`，任何地方都
**不要再手写**这张表。

为什么改成从 CSV 派生
--------------------
这张表原先在 `gen_gen3_data.py` 与 `regen_types.py` 里各手写了一份，
而且 `regen_types.py` 的「回归断言」是照着同一份手写表写的。结果是
**错表和错断言互相印证**，`make test` 全绿，实际有 14 处与官方不符：

    位置                当时写的   官方
    Normal -> Steel        1.0x    0.5x
    Flying -> Grass        1.0x    2.0x
    Poison -> Grass        1.0x    2.0x
    Rock   -> Fighting     2.0x    0.5x
    Bug    -> Fairy        1.0x    0.5x
    Steel  -> Flying       0.5x    1.0x
    Steel  -> Poison       0.0x    1.0x   （混淆了攻守方向：是毒打不动钢）
    Steel  -> Bug          0.5x    1.0x
    Steel  -> Water        1.0x    0.5x
    Steel  -> Electric     1.0x    0.5x
    Fire   -> Steel        0.5x    2.0x   （最严重：火打钢本来克制）
    Water  -> Grass        2.0x    0.5x
    Grass  -> Bug          2.0x    0.5x
    Psychic-> Fairy        2.0x    1.0x

手写 + 自证式断言这条路已经证伪，所以数据只认上游 CSV，
校验也直接对 CSV 做（见 tools/check_consistency.py）。

世代口径
--------
Veekun 这张表是最新一代（Gen VI+）的值，与本项目选定的规则一致：
含 Fairy；幽灵/恶打钢为 1.0x（Gen VI 起取消了大钢的抗性）。
两种倍率换算：CSV 的 damage_factor 单位是 1/100（200=2x），
对外统一用 1/10（20=2x），与 C 侧 kTypeEffect 的存储一致。
"""

import csv
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
CSV_PATH = ROOT / "data" / "csv" / "type_efficacy.csv"

# 属性顺序 = Veekun 的 type_id 顺序（1..18），索引 = type_id - 1。
# 这个顺序同时是 C 侧 TYPE_* 枚举的顺序，两边必须一致。
TYPES = [
    "Normal", "Fighting", "Flying", "Poison", "Ground", "Rock",
    "Bug", "Ghost", "Steel", "Fire", "Water", "Grass",
    "Electric", "Psychic", "Ice", "Dragon", "Dark", "Fairy",
]

N = len(TYPES)
# 18x18 全量矩阵：官方 CSV 每个组合一行，一条不多一条不少
EXPECTED_ROWS = N * N
# 单倍率，单位 1/10
NEUTRAL = 10

# 生成文件的首行。两个生成器（gen_gen3_data.py / regen_types.py）
# 共用同一份，走哪条路径产出的 poke_types.{h,c} 都是逐字节相同的。
BANNER = ("/* 自动生成：克制表来自 data/csv/type_efficacy.csv，"
          "改数据请改该 CSV（见 tools/type_chart.py） */")


def load_matrix():
    """读 CSV，返回 18x18 的列表（单位 1/10，20 = 2x）。

    矩阵是 [攻方][守方]，攻方在前 —— 与 games 内所有克制判断、
    与 C 侧 kTypeEffect 的约定一致。
    """
    if not CSV_PATH.exists():
        raise FileNotFoundError(
            f"缺少 {CSV_PATH}。这张表已随仓库入库（见 docs/data-sources.md），"
            f"不该缺失；若确实被删，从 Veekun CSV 重新拷一份。")

    matrix = [[NEUTRAL] * N for _ in range(N)]
    seen = set()
    with CSV_PATH.open(newline="", encoding="utf-8") as fp:
        for row in csv.DictReader(fp):
            atk = int(row["damage_type_id"]) - 1
            dfn = int(row["target_type_id"]) - 1
            # 先做边界检查再索引：out-of-range 在 Python 里是负数回绕，
            # 会静默把数据写到错的格子上
            if not (0 <= atk < N and 0 <= dfn < N):
                raise ValueError(f"type_id 越界: {atk + 1}, {dfn + 1}")
            if (atk, dfn) in seen:
                raise ValueError(f"CSV 里组合重复: {TYPES[atk]} -> {TYPES[dfn]}")
            seen.add((atk, dfn))
            # 100 -> 10、200 -> 20、50 -> 5、0 -> 0
            matrix[atk][dfn] = int(row["damage_factor"]) // 10

    if len(seen) != EXPECTED_ROWS:
        raise ValueError(
            f"CSV 只有 {len(seen)} 个组合，应为 {EXPECTED_ROWS} 个"
            f"（下载被截断？见 docs/data-sources.md 的坑）")
    return matrix


def idx(name):
    return TYPES.index(name)


def effectiveness(matrix, atk, defender_types):
    """把多个守方属性乘起来（双属性最多两个）。单位 1/10。"""
    k = idx(atk)
    out = NEUTRAL
    for d in defender_types:
        out = out * matrix[k][idx(d)] // NEUTRAL
    return out


def eff(atk, defender_types):
    """给人看的倍率（1.0 / 0.5 / 0.25 / 4.0 …）。校验用。"""
    return effectiveness(load_matrix(), atk, defender_types) / 10.0
