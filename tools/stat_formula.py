"""
stat_formula.py - 能力值 / 等级倍率 / 性格的**唯一权威来源**

为什么要有这个文件
------------------
这个项目里已经踩过三次同一类坑：**手写数据或公式 + 没有上游校验 +
测试照着实现写期望值** → 全绿但是错的。

    1. 属性克制表      14 格错（已改由 data/csv/type_efficacy.csv 生成）
    2. 能力值公式      IV 贡献砍半、努力值乘在等级缩放之外（全游戏偏移）
    3. 能力等级倍率表  13 档错 11 档，所有负档降能力失效
    4. 性格效果表      spa/spd 写反 4 个；中文名按错位的 id 取，25 错 24

所以这里把「能力相关的数值模型」集中到一处，并且：

  * 公式用 Python 独立重写一遍（不是把 C 抄过来），
  * `KNOWN_STATS` 里的期望值是**游戏外可查的已知数值**（烈咬陆鲨
    Lv100 252 速爽朗 = 333 之类），不是从本实现推出来的，
  * `self_check()` 断言「本实现能重算出这些已知值」，
  * `tools/gen_stat_golden.py` 把同一批已知值写成 `gen/poke_stat_golden.h`，
    C 侧测试（test_calc.c）直接拿它们断言。

于是要让错误溜过去，得同时错三处：C 实现、这个 Python 实现、以及手抄的
已知值。属性克制表用的是同一套思路。

数值口径
--------
stat 序号：hp(1) atk(2) def(3) spa(4) spd(5) spe(6)，与 poke_calc.h 一致。
公式一律整数、截断取整（官方的 floor），不四舍五入。
"""

from fractions import Fraction

MAX_IV = 31
MAX_EV = 255
MAX_STAGE = 6


# ============================================================
# 能力等级倍率（官方表，与代无关）
# ============================================================
# base = 2 用于能力值：+1 -> 3/2
# base = 3 用于命中/闪避：+1 -> 4/3
# 净等级钳到 ±6。
STAGE_MUL = {
    -6: Fraction(2, 8), -5: Fraction(2, 7), -4: Fraction(2, 6),
    -3: Fraction(2, 5), -2: Fraction(2, 4), -1: Fraction(2, 3),
     0: Fraction(1, 1),
     1: Fraction(3, 2),  2: Fraction(4, 2),  3: Fraction(5, 2),
     4: Fraction(6, 2),  5: Fraction(7, 2),  6: Fraction(8, 2),
}

ACC_STAGE_MUL = {
    -6: Fraction(3, 9), -5: Fraction(3, 8), -4: Fraction(3, 7),
    -3: Fraction(3, 6), -2: Fraction(3, 5), -1: Fraction(3, 4),
     0: Fraction(1, 1),
     1: Fraction(4, 3),  2: Fraction(5, 3),  3: Fraction(6, 3),
     4: Fraction(7, 3),  5: Fraction(8, 3),  6: Fraction(9, 3),
}

STAGES = list(range(-MAX_STAGE, MAX_STAGE + 1))


def stage_multiplier(stage, base=2):
    """官方倍率，返回 Fraction。"""
    tbl = STAGE_MUL if base == 2 else ACC_STAGE_MUL
    s = max(-MAX_STAGE, min(MAX_STAGE, int(stage)))
    return tbl[s]


def apply_stage(v, stage, base=2):
    """v 按等级倍率缩放，**截断**取整（官方对 100 给出 66 而不是 67）。"""
    if stage == 0:
        return v
    if base == 2:
        n = abs(stage)
        num, den = (n + 2, 2) if stage > 0 else (2, n + 2)
    else:
        n = abs(stage)
        num, den = (n + 3, 3) if stage > 0 else (3, n + 3)
    return (v * num) // den


# ============================================================
# 性格：id -> 英文名 / 英文名 -> (增加项, 减少项)
# ============================================================
# id 顺序沿用**游戏内顺序**（玩家熟悉，且 kNatures 的下标就是它）。
# 注意 Veekun 的 nature_id 是另一套顺序（1=hardy 2=bold 3=modest ...），
# 所以中文名必须按英文名配对，不能按 id 取。
NATURE_ORDER = {
    1: "hardy", 2: "lonely", 3: "brave", 4: "adamant", 5: "naughty",
    6: "bold", 7: "docile", 8: "relaxed", 9: "impish", 10: "lax",
    11: "timid", 12: "hasty", 13: "serious", 14: "jolly", 15: "naive",
    16: "modest", 17: "mild", 18: "quiet", 19: "bashful", 20: "rash",
    21: "calm", 22: "gentle", 23: "sassy", 24: "careful", 25: "quirky",
}

NATURE_EFFECT = {
    "hardy":   (0, 0), "lonely":  (2, 3), "brave":   (2, 6),
    "adamant": (2, 4), "naughty": (2, 5),
    "bold":    (3, 2), "docile":  (0, 0), "relaxed": (3, 6),
    "impish":  (3, 4), "lax":     (3, 5),
    "timid":   (6, 2), "hasty":   (6, 3), "serious": (0, 0),
    "jolly":   (6, 4), "naive":   (6, 5),
    "modest":  (4, 2), "mild":    (4, 3), "quiet":   (4, 6),
    "bashful": (0, 0), "rash":    (4, 5),
    "calm":    (5, 2), "gentle":  (5, 3), "sassy":   (5, 6),
    "careful": (5, 4), "quirky":  (0, 0),
}


def nature_pair(nature_id):
    """kTatures 下标 -> (增加项, 减少项)。0 号是占位。"""
    if nature_id <= 0:
        return (0, 0)
    return NATURE_EFFECT[NATURE_ORDER[nature_id]]


# ============================================================
# 能力值公式（Gen III 起未变）
# ============================================================
def calc_hp(base, iv, ev, level):
    """floor((2*Base + IV + floor(EV/4)) * Level / 100) + Level + 10"""
    return (2 * base + iv + ev // 4) * level // 100 + level + 10


def calc_stat(base, iv, ev, level, mult=Fraction(1, 1)):
    """floor(floor((2*Base + IV + floor(EV/4)) * Level / 100) + 5) * nature"""
    v = (2 * base + iv + ev // 4) * level // 100 + 5
    return (v * mult.numerator) // mult.denominator


def nature_mult(nature_id, stat_idx):
    inc, dec = nature_pair(nature_id)
    if inc == stat_idx:
        return Fraction(11, 10)
    if dec == stat_idx:
        return Fraction(9, 10)
    return Fraction(1, 1)


# ============================================================
# 外部已知值（**这是全文件的锚点**）
# ============================================================
# 每条都注明出处。种族值为官方数据；期望值取自游戏内可复现的数值
# （对战圈长期引用的常见配置）或按官方式手算 —— 但公式本身来自
# 官方资料，不是从本仓库的实现反推的。
#
# 字段：note / species_id / base(6 项) / level / nature / iv / ev(6 项) / want(6 项)
KNOWN_STATS = [
    dict(
        note="妙蛙种子 Lv50 满IV 0努力 中性（种族 45/49/49/65/65/45）",
        species_id=1, base=(45, 49, 49, 65, 65, 45),
        level=50, nature=1, iv=31, ev=(0, 0, 0, 0, 0, 0),
        want=(120, 69, 69, 85, 85, 65),
    ),
    dict(
        note="喷火龙 Lv50 满IV 0努力 中性（种族 78/84/78/109/85/100）",
        species_id=6, base=(78, 84, 78, 109, 85, 100),
        level=50, nature=1, iv=31, ev=(0, 0, 0, 0, 0, 0),
        want=(153, 104, 98, 129, 105, 120),
    ),
    # ---- 下面几条是 Lv100 满配置，数值在对战圈长期被引用，便于外部核对 ----
    dict(
        note="巨金怪 Lv100 252努力攻击 固执(+攻-特攻) —— 攻击 405 是三代对战常识值（种族 80/135/130/95/90/70）",
        species_id=376, base=(80, 135, 130, 95, 90, 70),
        level=100, nature=4, iv=31, ev=(0, 252, 0, 0, 0, 0),
        want=(301, 405, 296, 203, 216, 176),
    ),
    dict(
        note="暴飞龙 Lv100 252努力速度 爽朗(+速-特攻) —— 速度 328 是三代对战常识值（种族 95/135/80/110/80/100）",
        species_id=373, base=(95, 135, 80, 110, 80, 100),
        level=100, nature=14, iv=31, ev=(0, 0, 0, 0, 0, 252),
        want=(331, 306, 196, 230, 196, 328),
    ),
    dict(
        note="耿鬼 Lv100 252努力速度 胆小(+速-攻) —— 速度 350 是三代对战常识值（种族 60/65/60/130/75/110）",
        species_id=94, base=(60, 65, 60, 130, 75, 110),
        level=100, nature=11, iv=31, ev=(0, 0, 0, 0, 0, 252),
        want=(261, 149, 156, 296, 186, 350),
    ),
    dict(
        note="班基拉斯 Lv100 252努力HP 中性 —— HP 404 是对战常识值（种族 100/134/110/95/100/61）",
        species_id=248, base=(100, 134, 110, 95, 100, 61),
        level=100, nature=1, iv=31, ev=(252, 0, 0, 0, 0, 0),
        want=(404, 304, 256, 226, 236, 158),
    ),
    dict(
        note="幸福蛋 Lv100 252努力HP 中性 —— HP 714 是全游戏最高满血（种族 255/10/10/75/135/55）",
        species_id=242, base=(255, 10, 10, 75, 135, 55),
        level=100, nature=1, iv=31, ev=(252, 0, 0, 0, 0, 0),
        want=(714, 56, 56, 186, 306, 146),
    ),
    dict(
        note="妙蛙种子 Lv50 252努力攻击 —— 验证努力值乘在 ×Level/100 之内（101 而非 132）",
        species_id=1, base=(45, 49, 49, 65, 65, 45),
        level=50, nature=1, iv=31, ev=(0, 252, 0, 0, 0, 0),
        want=(120, 101, 69, 85, 85, 65),
    ),
    dict(
        note="妙蛙种子 Lv50 255努力攻击 —— 255//4=63，与 252 同值",
        species_id=1, base=(45, 49, 49, 65, 65, 45),
        level=50, nature=1, iv=31, ev=(0, 255, 0, 0, 0, 0),
        want=(120, 101, 69, 85, 85, 65),
    ),
]

# v=100 时的等级倍率结果表（下标 = stage + 6）。手写，取自官方倍率。
STAGE_MUL_AT_100 = (25, 28, 33, 40, 50, 66, 100, 150, 200, 250, 300, 350, 400)
# 命中/闪避用 base=3：-1 是 75（不是能力值的 66），+1 是 133（不是 150）
ACC_MUL_AT_100 = (33, 37, 42, 50, 60, 75, 100, 133, 166, 200, 233, 266, 300)


# ============================================================
# 自检
# ============================================================
def self_check(verbose=False):
    """返回 (ok, 消息列表)。断言本实现能重算出上面那些**外部已知值**。"""
    msgs = []
    ok = True

    def chk(cond, msg):
        nonlocal ok
        if not cond:
            ok = False
            msgs.append("!! " + msg)
        elif verbose:
            msgs.append("   " + msg)

    # --- 等级倍率表 ---
    for i, stage in enumerate(STAGES):
        got = apply_stage(100, stage, 2)
        chk(got == STAGE_MUL_AT_100[i],
            f"能力等级 {stage:+d}：100 -> {got}（应 {STAGE_MUL_AT_100[i]}）")
        got3 = apply_stage(100, stage, 3)
        chk(got3 == ACC_MUL_AT_100[i],
            f"命中等级 {stage:+d}：100 -> {got3}（应 {ACC_MUL_AT_100[i]}）")
    # 负数档必须真的减（历史上全塌成 1.0x，降能力完全无效）
    for stage in range(-MAX_STAGE, 0):
        chk(apply_stage(100, stage) < 100,
            f"能力等级 {stage} 应真的降低能力（<100）")
    for stage in range(1, MAX_STAGE + 1):
        chk(apply_stage(100, stage) > 100,
            f"能力等级 +{stage} 应真的提高能力（>100）")

    # --- 性格表 ---
    chk(len(NATURE_ORDER) == 25, "性格 id 恰好 25 个")
    chk(set(NATURE_ORDER.values()) == set(NATURE_EFFECT),
        "性格顺序表与效果表的英文名集合一致")
    ups = sorted({v[0] for v in NATURE_EFFECT.values() if v[0]})
    downs = sorted({v[1] for v in NATURE_EFFECT.values() if v[1]})
    chk(ups == [2, 3, 4, 5, 6] and downs == [2, 3, 4, 5, 6],
        f"性格的升/降项各覆盖 atk..spe（+{ups} -{downs}）")
    # 特攻(4)/特防(5) 曾经互换：固执本该 -特攻
    chk(nature_pair(4) == (2, 4), "固执(+atk -spa) 的特攻/特防没写反")
    chk(nature_pair(5) == (2, 5), "顽皮(+atk -spd) 的特攻/特防没写反")
    chk(nature_pair(9) == (3, 4), "淘气(+def -spa) 的特攻/特防没写反")
    chk(nature_pair(10) == (3, 5), "乐天(+def -spd) 的特攻/特防没写反")

    # --- 能力值已知值 ---
    # 本项目的数据表只到三代（图鉴 1..386）。写了不存在的编号时，C 侧
    # base_stat_of() 找不到就一律返回 50 —— 算出来「有值」但全是错的，
    # 不崩不报错。这里先挡一道（曾经真的写过 #445 烈咬陆鲨，而表里没有它）。
    for e in KNOWN_STATS:
        chk(1 <= e["species_id"] <= 386,
            f"species_id {e['species_id']} 落在三代图鉴范围内（1..386）")

    for e in KNOWN_STATS:
        got = [
            calc_hp(e["base"][0], e["iv"], e["ev"][0], e["level"]),
            calc_stat(e["base"][1], e["iv"], e["ev"][1], e["level"], nature_mult(e["nature"], 2)),
            calc_stat(e["base"][2], e["iv"], e["ev"][2], e["level"], nature_mult(e["nature"], 3)),
            calc_stat(e["base"][3], e["iv"], e["ev"][3], e["level"], nature_mult(e["nature"], 4)),
            calc_stat(e["base"][4], e["iv"], e["ev"][4], e["level"], nature_mult(e["nature"], 5)),
            calc_stat(e["base"][5], e["iv"], e["ev"][5], e["level"], nature_mult(e["nature"], 6)),
        ]
        want = list(e["want"])
        chk(got == want, f"{e['note'][:28]}… -> {got}（应 {want}）")

    return ok, msgs


if __name__ == "__main__":
    good, out = self_check(verbose=True)
    for m in out:
        print(m)
    print("=== stat_formula 自检: %s ===" % ("通过" if good else "失败"))
    raise SystemExit(0 if good else 1)
