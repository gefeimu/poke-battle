"""
从 Veekun CSV 生成三代宝可梦对战完整数据表（C 头文件 + 源文件）。

数据源：PokeAPI/pokeapi data/v2/csv（Veekun 数据，CC0/GPL 双许可，此处仅自用学习）
覆盖：种族值 / 属性 / 特性 / 性格 / 努力值成长 / 招式 / 学习表 / 道具 / Mega

设计要点：
- 只保留三代（Gen 1-3，national_index 1..386）相关内容，控制体积
- 招式含 effect_id，映射到本地的 effect_kind 枚举，引擎按 kind 分派
- 属性 18 种完整克制表由 CSV 的 damage_relations 派生（见 gen_types.py）
- 表结构固定宽度，便于 ESP32 上直接索引

用法：
    python tools/gen_gen3_data.py
"""

import argparse
import csv
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent

# zh_names 与本脚本同目录，import 前先确保 tools 在 sys.path 里
# （用 `python tools/gen_gen3_data.py` 运行时脚本目录不一定已注册）
if str(ROOT / "tools") not in sys.path:
    sys.path.insert(0, str(ROOT / "tools"))
from zh_names import load_names
CSV = ROOT / "data" / "csv"
GEN = ROOT / "gen"
GEN.mkdir(parents=True, exist_ok=True)

# 三代范围（含 Mega 但 Mega 用独立 form id 10034..10134 段）
GEN3_MAX = 386
MEGA_MIN=10034
MEGA_MAX = 10134

STAT_ORDER = ["hp", "attack", "defense", "special-attack", "special-defense", "speed"]
STAT_ID_MAP = {"hp": 1, "attack": 2, "defense": 3, "special-attack": 4, "special-defense": 5, "speed": 6}

# 三代实际存在的属性（1..18）
TYPE_IDS = list(range(1, 19))

# 三代性格ID（1..25）
NATURE_IDS = list(range(1, 26))


# ============================================================
# 属性回退表（仅 GEN<=5 规则启用）
# ============================================================
# Veekun CSV 是全世代数据，宝可梦与招式的属性按"最新一代"（Gen 6+）记录。
# 规则世代由命令行 --gen 决定（默认 6）：
#
#   --gen 6  第六代 X/Y ORAS：**启用 Fairy**，不启用本回退表，
#            直接采用 Veekun 的属性。这是本项目当前选定的规则。
#   --gen 3  第三代：启用本回退表，把 Fairy 相关条目改回三代属性。
#   --gen 5  第五代黑/白：与 --gen 3 等价（Gen 2 加钢后到 Gen 5
#            为止这批宝可梦属性未再变），同样启用回退表。
#
# 历史背景——这批属性变更发生过两次：
#
#   第六世代（X/Y）引入 Fairy 属性，改了 18 只一~三代宝可梦：
#     皮皮/胖丁 Clefairy/Clefable   纯超能-> Fairy
#     胖丁胖胖 Wigglytuff            纯一般-> Normal/Fairy
#     胖丁 Jigglypuff                纯一般-> Normal/Fairy
#     皮宝宝 Cleffa / Igglybuff      纯一般-> Fairy / Normal+Fairy
#     魔幻假面喵 Mr. Mime超能/格斗-> Psychic/Fairy
#     波克比 Togepi                 纯一般-> Fairy
#     波克鸟 Togetic                一般/飞行-> Fairy/Flying
#     玛力露 Marill / 露力丽 Azumarill / 露力露 Azurill 纯水-> Water/Fairy
#     圈圈熊 Snubbull / 布咕顿 Granbull 纯一般-> Fairy
#     拉鲁拉丝 Ralts / 奇拉莉 Kirlia / 沙奈朵 Gardevoir  纯超能-> Psychic/Fairy
#     大钳螳螂 Mawile               纯钢-> Steel/Fairy
#
#   同时 3 个一般属性招式被改成 Fairy：撒娇 Charm / 甜吻 Sweet Kiss / 月光 Moonlight
#
# 另外 Gen 2 引入钢属性时，磁力怪/自爆磁怪从纯电变成电/钢——
# 这一条不用回退，Veekun 记录的已经是电/钢，三代同样是电/钢。
#
# 下面这张表把三代范围内的条目改回三代真实属性。
# 键是 pokemon_id（1..386 与图鉴号一致；Mega 用其 pokemon id）。
TYPE_OVERRIDE_PRE_GEN6 = {
    35:  ("Psychic", "Psychic"),   # 皮皮 Clefairy
    36:  ("Psychic", "Psychic"),   # 胖丁 Clefable
    39:  ("Normal", "Normal"),# 胖丁 Jigglypuff
    40:  ("Normal", "Normal"),     # 胖丁胖胖 Wigglytuff
    122: ("Psychic", "Fighting"),  # 魔幻假面喵 Mr. Mime（三代是超能/格斗）
    173: ("Normal", "Normal"),     # 皮宝宝 Cleffa
    174: ("Normal", "Normal"),     # 皮皮宝宝 Igglybuff
    175: ("Normal", "Normal"),     # 波克比 Togepi
    176: ("Normal", "Flying"),     # 波克鸟 Togetic
    183: ("Water", "Water"),       # 玛力露 Marill
    184: ("Water", "Water"),       # 露力丽 Azumarill
    209: ("Normal", "Normal"),     # 圈圈熊 Snubbull
    210: ("Normal", "Normal"),     # 布咕顿 Granbull
    280: ("Psychic", "Psychic"),   # 拉鲁拉丝 Ralts
    281: ("Psychic", "Psychic"),   # 奇拉莉 Kirlia
    282: ("Psychic", "Psychic"),   # 沙奈朵 Gardevoir
    298: ("Normal", "Normal"),     # 露力露 Azurill
    303: ("Steel", "Steel"),       # 大钳螳螂 Mawile
    # ---- Mega 形态 ----
    # Mega 是第六世代的形态，其属性本就以第六代为准，
    # 但 Mega Gardevoir / Mega Mawile 的 Fairy 来自第六代的属性重分类，
    # 三代语境下这两只不存在 Fairy，因此仍按 Gen 5 形态属性记录。
    10051: ("Psychic", "Psychic"),  # Mega Gardevoir
    10052: ("Steel", "Steel"),      # Mega Mawile
    10067: ("Dragon", "Flying"),    # Mega Altaria（龙/飞行，非龙/妖精）
}

# 招式属性回退：键是 move id（Veekun moves.csv 的编号）
# 值与 type_index 键风格一致 —— 首字母大写。
MOVE_TYPE_OVERRIDE_PRE_GEN6 = {
    186: "Normal",   # Sweet Kiss 甜吻
    204: "Normal",   # Charm      撒娇
    236: "Normal",   # Moonlight   月光
}

# ============================================================
# 官方 Mega 形态白名单
# ============================================================
# Veekun 的 pokemon表 / pokemon_forms 表在 10000+ 段混入了一批
# 官方从未实装的"Mega"占位记录（游戏 ROM 里残留的未启用数据，
# 后来被数据源当成正式形态收录了）。实测会多出：
#   clefable_mega / victreebel_mega / starmie_mega / dragonite_mega /
#   meganium_mega / feraligatr_mega / skarmory_mega / chimecho_mega /
#   raichu_mega_x / raichu_mega_y / absol_mega_z / *_mega_z等
#
# 只有 is_default=1 且 is_battle_only=1 也不够（占位记录同样满足）。
# 唯一可靠做法是硬编码官方名单—— Mega 进化在游戏里是有限且固定的，
# 白名单本身也是最稳定的接口，不会随数据源更新而变。
#
# 官方 Mega 形态共 48 个（X/Y 引入 30 + ORAS 引入 20，其中
# 喷火龙/ 超梦各有两个形态）。下面只保留三代图鉴内的部分。
#
# 结构：基础种图鉴号 -> Mega pokemon id 列表
MEGA_WHITELIST = {
    # ---- 第六世代 X/Y 引入 ----
    3:   [10033],               # 妙蛙种子 Venusaur
    6:   [10034, 10035],        # 喷火龙 Charizard（X / Y 双形态）
    9:   [10036],               # 水箭龟 Blastoise
    65:  [10037],               # 胡地 Alakazam
    94:  [10038],               # 耿鬼 Gengar
    115: [10039],               # 袋兽 Kangaskhan
    127: [10040],               # 大针蜂 Pinsir
    130: [10041],               # 暴鲤龙 Gyarados
    142: [10042],               # 化石翼龙 Aerodactyl
    150: [10043, 10044],        # 超梦 Mewtwo（X / Y 双形态）
    181: [10045],               # 雷公 Ampharos
    212: [10046],               # 巨钳螳螂 Scizor
    214: [10047],               # 独角大盗 Heracross
    229: [10048],               # 黑鲁莽 Houndoom
    248: [10049],               # 烈咬鲲 Tyranitar
    257: [10050],               # 火焰鸡 Blaziken
    282: [10051],               # 沙奈朵 Gardevoir
    303: [10052],               # 大嘴娃 Mawile
    306: [10053],               # 铁甲龙 Aggron
    308: [10054],               # 沙力那奥 Medicham
    310: [10055],               # 雷电兽 Manectric
    354: [10056],               # 梦妖 Banette
    359: [10057],               # 阿勃梭鲁 Absol
    380: [10062],               # 拉帝亚斯 Latias
    381: [10063],               # 拉帝欧斯 Latios
    # ---- 第六世代 ORAS 引入 ----
    15:  [10090],               # 大针蜂 Beedrill
    18:  [10073],               # 快速鹏 Pidgeot
    80:  [10071],               # 呆呆兽 Slowbro
    208: [10072],               # 大钢蛇 Steelix
    254: [10065],               # 森林蜥 Sceptile
    260: [10064],               # 巨藻 Swampert
    302: [10066],               # 叉字蝠 Sableye
    319: [10070],               # 猎咬鱼 Sharpedo
    323: [10087],               # 喷火肚 Camerupt
    334: [10067],               # 七夕青鸟 Altaria
    362: [10074],               # 雪妖女 Glalie
    373: [10089],               # 烈空坐 Salamence
    376: [10076],               # 巨金 Metagross
    # 烈空坐 Rayquaza (#384) 的 Mega 形态 pid 10079 也在三代图鉴内，
    # 但它不使用 Mega 石，而是由「龙之波动」在战斗中直接触发。
    # 这是 Mega 进化的唯一特例，引擎实现时要单独处理。
    384: [10079],               # 烈空坐 Rayquaza（无 Mega 石，靠招式触发）
}

# 白名单里出现的所有 Mega pokemon id
MEGA_PID_SET = {pid for pids in MEGA_WHITELIST.values() for pid in pids}

# 官方 Mega 石名录（只含三代图鉴内的 39 块）。
#
# 有意偏离：Mega 石在 Veekun 里被记为第 6 代（首发于 X/Y），
# 但本项目要在三代规则里实现 Mega 进化，因此强行并入道具表。
#
# 两个容易记错的点：
#   1) 独角大盗的石头英文名是 Heracronite（不是 Heracrossite，
#      后者是意大利语/西语译名）。Veekun 与 PokeAPI 都用 Heracronite。
#   2) 烈空坐的 Mega 进化不需要 Mega 石——它由「龙之波动」
#      在战斗中直接触发，是 Mega 进化的唯一特例。
#      所以白名单里没有 Rayquazite，这也是 Veekun 查不到它的原因。
#
# 交叉验证：Veekun 中所有 *ite 结尾道具里，有 42 个没有
# item_game_indices 记录（Veekun 用这个标记「官方未实装」），
# 恰好对应官方不存在的 Mega 石（Clefablite / Victreebelite /
# Starminite / Dragoninite / Meganiumite / Feraligite / Skarmorite /
# Raichunite X/Y / Chimechite / *ite Z 等），与本白名单互补。
#
# 不能用 "ite" 后缀匹配：Meteorite（陨石 id 537）、Black Augurite、
# Sablenite 等非 Mega 道具也以 ite 结尾，会被误收。
MEGA_STONE_NAMES = {
    # ---- 第六世代 X/Y ----
    "Venusaurite", "Charizardite X", "Charizardite Y", "Blastoisinite",
    "Alakazite", "Gengarite", "Kangaskhanite", "Pinsirite",
    "Gyaradosite", "Aerodactylite", "Mewtwonite X", "Mewtwonite Y",
    "Ampharosite", "Scizorite", "Heracronite", "Houndoominite",
    "Tyranitarite", "Blazikenite", "Gardevoirite", "Mawilite",
    "Aggronite", "Medichamite", "Manectite", "Banettite", "Absolite",
    "Latiasite", "Latiosite",
    # ---- 第六世代 ORAS ----
    "Beedrillite", "Pidgeotite", "Slowbronite", "Steelixite",
    "Sceptilite", "Swampertite", "Sharpedonite", "Cameruptite",
    "Altarianite", "Glalitite", "Salamencite", "Metagrossite",
}


def load(name, required=True):
    p = CSV / f"{name}.csv"
    if not p.exists():
        if required:
            raise FileNotFoundError(f"缺少 {p}，请先运行 tools/fetch_csv.py")
        return []
    with open(p, encoding="utf-8", newline="") as fh:
        return list(csv.DictReader(fh))


def to_int(v, default=0):
    if v is None or v == "":
        return default
    try:
        return int(v)
    except ValueError:
        return default


def cident(s):
    """转成合法 C 标识符。"""
    out = []
    for ch in s:
        out.append(ch if (ch.isalnum() or ch == "_") else "_")
    r = "".join(out)
    if r and r[0].isdigit():
        r = "_" + r
    return r or "x"


def main():
    ap = argparse.ArgumentParser(
        description="Veekun CSV -> C 数据表（宝可梦图鉴范围 1..386 + Mega）")
    ap.add_argument("--gen", type=int, default=6, choices=[3, 5, 6],
                    help="规则世代。6=第六代 X/Y ORAS（默认，启用 Fairy 与 Mega）；"
                         "3 或 5=回退 Fairy 相关属性到 Gen5 及以前的形态")
    args = ap.parse_args()
    rule_gen = args.gen
    # Fairy 是第六世代引入的。规则世代 <6 时必须启用属性回退表，
    # 否则会把 Gen6 的属性变更（皮皮变 Fairy 等）带进 Gen3/5 规则，
    # 导致「钢系打皮皮=0x无效」这类该世代不存在的克制结论。
    use_pre_gen6_types = (rule_gen < 6)

    print(f"规则世代: 第 {rule_gen} 代"
          f"（{'启用 Fairy，不启用属性回退表' if rule_gen >= 6 else '启用属性回退表，屏蔽 Fairy'}）")

    print("加载 CSV ...")
    species_all = load("pokemon_species")
    pokemon_all = load("pokemon")
    stats_all = load("pokemon_stats")
    types_all = load("pokemon_types")
    moves_all = load("moves")
    moves_meta = load("move_meta")
    effects_all = load("move_effects")
    pmoves_all = load("pokemon_moves")
    abilities_all = load("abilities")
    ability_names = load("ability_names", required=False)
    forms_all = load("pokemon_forms")

    print(f"  species={len(species_all)} pokemon={len(pokemon_all)} moves={len(moves_all)}")

    # ---------- 属性名映射 ----------
    # type_names.csv 每种属性对多种语言都有行，只有英语行能给出英文名。
    # 若英语行缺失（少数属性），退化为用Veekun 的内部 id 建名，
    # 保证矩阵下标与 kTypeNames 一致，不因缺名而错位。
    type_names = {}
    for r in load("type_names"):
        if to_int(r["local_language_id"]) == 9:
            type_names[to_int(r["type_id"])] = r["name"]
    for tid in TYPE_IDS:
        if tid not in type_names:
            type_names[tid] = f"type{tid}"

    # ---------- 特性名映射 ----------
    ability_names_map = {}
    for r in ability_names:
        if to_int(r["local_language_id"]) == 9:
            ability_names_map[to_int(r["ability_id"])] = r["name"]

    # ---------- 招式名 ----------
    move_names_map = {}
    for r in load("move_names"):
        if to_int(r["local_language_id"]) == 9:
            move_names_map[to_int(r["move_id"])] = r["name"]

    # ---------- 道具名 ----------
    item_names_map = {}
    for r in load("item_names"):
        if to_int(r["local_language_id"]) == 9:
            item_names_map[to_int(r["item_id"])] = r["name"]

    # ---------- 中文名（简体） ----------
    # Veekun 的 *_names.csv 每种语言一行，中文只有繁体（language_id=4），
    # 没有简体行。繁体转简体用 tools/zh_names.py 里的 Windows LCMapStringEx，
    # 不引入第三方依赖（见该文件的说明）。
    global zh_species, zh_moves, zh_types, zh_abilities, zh_items, zh_natures
    zh_species = load_names("species")
    zh_moves = load_names("moves")
    zh_types = load_names("types")
    zh_abilities = load_names("abilities")
    zh_items = load_names("items")
    zh_natures = load_names("natures")

    # ============================================================
    # 1) 属性表
    # ============================================================
    # Veekun 的 type_id 已是标准表序（1=Normal, 2=Fighting, ... 18=Fairy），
    # 直接按 id 递增建枚举，索引 = type_id - 1，避免任何重排导致错位。
    type_list = [(tid, type_names[tid]) for tid in TYPE_IDS]
    type_index = {name: i for i, (tid, name) in enumerate(type_list)}

    emit_types(type_list)

    # ============================================================
    # 2) 招式表（含 effect 分派）
    # ============================================================
    meta_by_move = {to_int(r["move_id"]): r for r in moves_meta}
    # 招式能力变化表：move_id -> [(stat_id, change), ...]
    stat_changes = {}
    for r in load("move_meta_stat_changes", required=False):
        stat_changes.setdefault(to_int(r["move_id"]), []).append(
            (to_int(r["stat_id"]), to_int(r["change"])))

    # ---------- 招式效果数据源 ----------
    # 只用 move_meta.csv 的结构化字段，**不用** moves.csv 的 effect_id：
    # Veekun 的 effect_id 是招式效果的文本条目编号，不是分类枚举。
    # 曾经按 effect_id 猜过一套 EFFECT_* 名字，实测全错位——
    # 空手切被标成 EFFECT_TRAPPING_ESCAPE、连环巴掌标成 EFFECT_TOXIC、
    # 剑舞标成 EFFECT_MAGNET_RISE。那是把"效果文本条目"当"效果类型"用，
    # 完全不能用。move_meta 的字段才是可靠的分类。
    #
    #   meta_category_id  招式大类（伤害/异常/数值变化/回复/...）
    #   meta_ailment_id   具体异常状态（麻痹/睡眠/冰冻/灼伤/中毒/混乱/...）
    #   ailment_chance    异常触发概率 %
    #   stat_chance       能力变化触发概率 %（0 = 必定触发）
    #   move_meta_stat_changes 具体改哪几项能力、改多少
    #   healing/drain/crit_rate/flinch_chance/min_hits/max_hits
    #
    # 本地枚举见 poke_moves.h 的 MoveCategory / MoveAilment。

    moves = {}
    for m in moves_all:
        mid = to_int(m["id"])
        gen = to_int(m["generation_id"])
        if gen > 3:
            continue  # 只保留 1-3 代
        tname = type_names.get(to_int(m["type_id"]), "")
        if tname not in type_index:
            continue
        # ---- 三代属性回退 ----
        # Veekun 的 moves.csv 是全世代数据，按"当前"属性记录。
        # 第六世代引入 Fairy 属性时，有 3 个原本一般属性的招式被改成了 Fairy。
        # 本项目规则是三代，必须用三代属性，否则克制关系会算错
        # （例如钢系打 Fairy=0x，但三代里这层关系根本不存在）。
        # 注意 type_index 的键是首字母大写（Normal / Fire / Fairy）。
        if use_pre_gen6_types:
            tname = MOVE_TYPE_OVERRIDE_PRE_GEN6.get(mid, tname)
            if tname not in type_index:
                raise SystemExit(f"招式 #{mid} 回退属性无效: {tname}")
        meta = meta_by_move.get(mid, {})
        # Veekun 的 damage_class_id 是 1/2/3 三值，不是 0/1/2：
        #   1 = status, 2 = physical, 3 = special
        # 本地枚举定义 0=status, 1=physical, 2=special，必须做映射。
        dc = to_int(m["damage_class_id"], 1)
        dmg_class = {1: 0, 2: 1, 3: 2}.get(dc, 0)
        eff = to_int(m["effect_id"], 0)  # 仅作调试参考，不参与战斗逻辑

        moves[mid] = {
            "id": mid,
            "name": cident(move_names_map.get(mid, f"move_{mid}")),
            "type": type_index[tname],
            "type_name": tname.upper(),
            "power": to_int(m["power"]),
            "pp": to_int(m["pp"]),
            "accuracy": to_int(m["accuracy"]),
            "priority": to_int(m["priority"]),
            "target": to_int(m["target_id"], 0),
            "damage_class": dmg_class,
            "effect_id": eff,          # 仅调试参考，勿用于逻辑
            "category": to_int(meta.get("meta_category_id"), 0),
            "ailment": to_int(meta.get("meta_ailment_id"), 0),
            "ailment_chance": to_int(meta.get("ailment_chance"), 0),
            "stat_chance": to_int(meta.get("stat_chance"), 0),
            "stat_changes": stat_changes.get(mid, []),
            "flinch_chance": to_int(meta.get("flinch_chance"), 0),
            "min_hits": to_int(meta.get("min_hits"), 1) or 1,
            "max_hits": to_int(meta.get("max_hits"), 1) or 1,
            "priority_bracket": to_int(meta.get("priority"), 0),
            "healing": to_int(meta.get("healing"), 0),
            "crit_rate": to_int(meta.get("crit_rate"), 0),
            "drain": to_int(meta.get("drain"), 0),
            "flinch_chance": to_int(meta.get("flinch_chance"), 0),
            "secondary_chance": to_int(meta.get("secondary_chance"), 0),
            "min_turns": to_int(meta.get("min_turns"), 0),
            "max_turns": to_int(meta.get("max_turns"), 0),
        }

    emit_moves(moves)

    # ============================================================
    # 3) 种族表（含 Mega）
    # ============================================================
    # pokemon.csv: id -> species_id
    # stats/pokemon_types/pokemon_moves 都以 pokemon_id 为键
    stats_by_pid = {}
    for r in stats_all:
        pid = to_int(r["pokemon_id"])
        stat = to_int(r["stat_id"])
        if stat in STAT_ID_MAP.values():
            stats_by_pid.setdefault(pid, {})[stat] = to_int(r["base_stat"])

    types_by_pid = {}
    for r in types_all:
        pid = to_int(r["pokemon_id"])
        slot = to_int(r["slot"])
        tn = type_names.get(to_int(r["type_id"]), "normal")
        if tn in type_index:
            types_by_pid.setdefault(pid, {})[slot] = type_index[tn]

    species_by_id = {to_int(r["id"]): r for r in species_all}
    growth_by_species = {}
    for s in species_all:
        growth_by_species[to_int(s["id"])] = to_int(s["growth_rate_id"], 1)

    # 三代 pokemon id 1..386 + 官方 Mega 形态
    # 规则：
    #   1) species_id <= 386 且 is_default=1  -> 386 个常规默认形态
    #   2) pokemon id 在 MEGA_PID_SET 里       -> 官方 Mega 形态
    #
    # 第2 条必须用硬编码白名单，不能靠 identifier 含 "-mega" 判断：
    # Veekun 的 10000+ 段混入了官方从未实装的占位 Mega 记录
    # （clefable-mega / victreebel-mega / raichu-mega-x / absol-mega-z 等），
    # 它们同样带 "-mega" 标识，光靠字符串匹配会多收十几个。
    valid_pids = []
    for p in pokemon_all:
        pid = to_int(p["id"])
        sid = to_int(p["species_id"])
        if sid > GEN3_MAX:
            continue
        if to_int(p["is_default"], 1) == 1:
            valid_pids.append(pid)
        elif pid in MEGA_PID_SET:
            valid_pids.append(pid)
    valid_pids.sort()

    species_out = []
    for pid in valid_pids:
        row = next((x for x in pokemon_all if to_int(x["id"]) == pid), None)
        if row is None:
            continue
        sid = to_int(row["species_id"])
        sp = species_by_id.get(sid)
        if sp is None:
            continue
        # Mega 形态的 identifier 形如 charizard-mega-x
        ident = row["identifier"]
        st = stats_by_pid.get(pid, {})
        # ---- 三代属性回退 ----
        # Veekun 记录的是最新一代属性，三代图鉴里 18 只宝可梦在 Gen 6
        # 被改成 Fairy，必须回退到三代真实属性，否则克制关系全错。
        if use_pre_gen6_types and pid in TYPE_OVERRIDE_PRE_GEN6:
            n1, n2 = TYPE_OVERRIDE_PRE_GEN6[pid]
            t1 = type_index[n1]
            t2 = type_index[n2]
        else:
            tp = types_by_pid.get(pid, {})
            t1 = tp.get(1, 0)
            t2 = tp.get(2, t1)
        # 反查属性枚举名
        tname_list = [name for _tid, name in type_list]
        species_out.append({
            "id": pid,
            "species_id": sid,
            "ident": cident(ident),
            "gen": to_int(sp["generation_id"], 3),
            "is_mega": 1 if pid in MEGA_PID_SET else 0,
            "type1": t1,
            "type2": t2,
            "type1_name": tname_list[t1].upper() if t1 < len(tname_list) else "",
            "type2_name": tname_list[t2].upper() if t2 < len(tname_list) else "",
            "hp": st.get(1, 50),
            "atk": st.get(2, 50),
            "def": st.get(3, 50),
            "spa": st.get(4, 50),
            "spd": st.get(5, 50),
            "spe": st.get(6, 50),
            "catch_rate": to_int(sp["capture_rate"], 45),
            "growth": growth_by_species.get(sid, 1),
            "gender_rate": to_int(sp["gender_rate"], -1),
        })

    # Mega -> 基础形态映射
    # Veekun 中 Mega 形态的 species_id 字段直接就是它所基于的图鉴编号
    # （如 charizard-mega-x 的 species_id = 6），可直接用作基础种。
    mega_map = {}
    for s in species_out:
        if s["is_mega"]:
            mega_map[s["id"]] = s["species_id"]

    emit_species(species_out, mega_map)

    # ============================================================
    # 4) 学习表（三代 level-up，按 version_group 过滤 red/blue..emerald）
    # ============================================================
    # 三代 version_group：red-blue(1) yellow(2) gold-silver(3) crystal(4)
    #   ruby-sapphire(5) emerald(6) firered-leafgreen(7) diamond(8)
    # 优先级：emerald(6) > firered-leafgreen(7) > ruby-sapphire(5) > red-blue(1)
    VG_PRIORITY = {6: 0, 7: 1, 5: 2, 1: 3, 2: 3, 4: 2}
    method_level = 1  # pokemon_move_methods: 1=level-up

    learn_by_pid = {}
    valid_pid_set = set(valid_pids)
    for r in pmoves_all:
        pid = to_int(r["pokemon_id"])
        # 直接用 valid_pid_set，不要经过 base_by_pid（Mega 的 species_id 也是基础种，
        # 会让形态与基础种的学习表互相污染）
        if pid not in valid_pid_set:
            continue
        if to_int(r["pokemon_move_method_id"]) != method_level:
            continue
        lvl = to_int(r["level"])
        if lvl <= 0:
            continue
        mid = to_int(r["move_id"])
        if mid not in moves:
            continue
        vg = to_int(r["version_group_id"])
        prio = VG_PRIORITY.get(vg, 9)
        if prio >= 9:
            continue
        key = (mid, lvl)
        cur = learn_by_pid.setdefault(pid, {})
        if key not in cur or prio < cur[key][1]:
            cur[key] = (lvl, prio)

    #汇总成列表，按等级排序
    learnsets = {}
    for pid, d in learn_by_pid.items():
        items = sorted(((mid, lvl) for (mid, lvl), _ in d.items()), key=lambda x: (x[1], x[0]))
        # 去重同等级
        dedup = []
        for mid, lvl in items:
            if dedup and dedup[-1][1] == lvl:
                dedup[-1] = (mid, lvl)
            else:
                dedup.append((mid, lvl))
        learnsets[pid] = dedup[:24]

    emit_learnsets(learnsets, valid_pids)

    # ============================================================
    # 5) 特性表
    # ============================================================
    # 特性范围：放宽到第 6 代。
    # 虽然宝可梦范围是三代图鉴，但 Mega 进化（第六代）会给宝可梦换上
    # 四代以后才引入的特性（Mega 喷火龙 X = Tough Claws、
    # Mega 沙奈朵 = Pixilate、Mega 大嘴娃 = Huge Power），
    # 只保留 Gen3 特性会导致 Mega 后特性全部丢失。
    # 代价是多约 175 个特性枚举，编译后约 700 字节，可接受。
    ABILITY_GEN_MAX = 6
    abil_out = []
    for a in abilities_all:
        aid = to_int(a["id"])
        gen = to_int(a["generation_id"])
        if gen > ABILITY_GEN_MAX:
            continue
        abil_out.append({
            "id": aid,
            "name": cident(ability_names_map.get(aid, f"ability_{aid}")),
        })
    emit_abilities(abil_out)

    # ============================================================
    # 6) 性格表
    # ============================================================
    emit_natures()

    # ============================================================
    # 7) 道具表
    # ============================================================
    items_all = load("pokemon_items")
    item_effects = {}
    for r in load("item_effects", required=False):
        item_effects[to_int(r["item_id"])] = to_int(r.get("healing"), 0)

    # 按"登场代<=3"筛道具：item_game_indices.generation_id 是 Veekun 记录的
    # 首次登场代，比按 item_id 区间猜要可靠得多。
    game_idx = load("item_game_indices")
    gen3_item_ids = {
        to_int(r["item_id"]) for r in game_idx
        if r.get("generation_id") and to_int(r["generation_id"]) <= 3
    }

    # 有意偏离：Mega 石在 Veekun 里被记为第6 代（首发于 X/Y），
    # 但本项目要在三代规则里实现 Mega 进化，因此强行并入。
    #
    # 白名单来自官方 Mega 石名录，且只取三代图鉴内的 40 块。
    # 不用关键词模糊匹配——那会误抓 Mewnium Z / Kangaskhan Candy /
    # Heracross Claw，也会把官方从未实装的 Clefablite / Victreebelite 收进来。
    #
    # 两个容易记错的点：
    #   1) 独角大盗的石头英文名是 Heracronite（不是 Heracrossite，
    #      后者是意大利语/西语译名）。Veekun 与 PokeAPI 都用 Heracronite。
    #   2) 烈空坐的 Mega 进化不需要 Mega 石——它由「龙之波动」
    #      在战斗中直接触发（Rayquaza 是唯一特殊的 Mega 触发条件）。
    #      因此白名单里没有 Rayquazite，这也是 Veekun 里查不到它的原因。
    #
    # 校验：Veekun 中所有 *ite 结尾道具里，有 42 个没有 item_game_indices
    # 记录（Veekun 用这个标记"官方未实装"），恰好对应官方不存在的
    # Mega 石（Clefablite / Victreebelite / Starminite / Dragoninite /
    # Meganiumite / Feraligite / Skarmorite / Raichunite X/Y /
    # Chimechite / *ite Z 等），与本白名单互为交叉验证。
    # MEGA_STONE_NAMES 见文件头（模块级，供 emit_items 里的 classify 使用）

    items_out = []
    seen = set()
    for iid in sorted(gen3_item_ids):
        name = item_names_map.get(iid, "")
        if not name:
            continue
        seen.add(iid)
        items_out.append({
            "id": iid,
            "name": name,
            "heal": item_effects.get(iid, 0),
        })

    # 补入三代不存在的 Mega 石
    mega_added = 0
    for iid, name in sorted(item_names_map.items()):
        if iid in seen:
            continue
        if name in MEGA_STONE_NAMES:
            items_out.append({
                "id": iid,
                "name": name,
                "heal": item_effects.get(iid, 0),
            })
            seen.add(iid)
            mega_added += 1
    items_out.sort(key=lambda x: x["id"])

    missing = MEGA_STONE_NAMES - {it["name"] for it in items_out}
    if missing:
        print(f"  [warn] 白名单里这些 Mega 石未在 Veekun 找到: {sorted(missing)}")
    print(f"  三代道具 {len(gen3_item_ids)} 个，额外并入 Mega 石 {mega_added} 个")
    emit_items(items_out)

    # 汇总报告
    print("\n生成完成：")
    total = 0
    for f in sorted(GEN.glob("*.h")) + sorted(GEN.glob("*.c")):
        if "sprite" in f.name:
            continue
        sz = f.stat().st_size
        total += sz
        print(f"  {f.name:24s} {sz:>8,} B")
    print(f"  {'合计':24s} {total:>8,} B  ({total/1024:.0f} KB)")


# ================================================================
# 发射器
# ================================================================

# 简体中文名表，由 main() 从 Veekun CSV 读入后填充，供各 emit_* 使用。
# 模块级而非函数参数：六个 emit 函数都要用，逐个传参太啰嗦。
zh_species = {}
zh_moves = {}
zh_types = {}
zh_abilities = {}
zh_items = {}
zh_natures = {}


def emit_types(type_list):
    lines = [
        "/* 由 tools/gen_gen3_data.py 自动生成 */",
        "#ifndef POKE_TYPES_H",
        "#define POKE_TYPES_H",
        "",
        "#include <stdint.h>",
        "",
        "typedef enum {",
    ]
    for i, (tid, name) in enumerate(type_list):
        lines.append(f"    TYPE_{name.upper()} = {i},")
    lines += ["    TYPE_COUNT", "} PokeType;", "",
              f"#define TYPE_MATRIX_SIZE {len(type_list)}",
              "",
              "/* [攻方][守方]，值 = 倍率 * 10（20=2x, 10=1x, 5=0.5x, 0=0x） */",
              f"extern const uint8_t kTypeEffect[TYPE_MATRIX_SIZE][TYPE_MATRIX_SIZE];",
              "extern const char *const kTypeNames[TYPE_COUNT];",
              "",
              "/* 简体中文属性名（索引与 TYPE_* 一致），UI 显示用 */",
              "extern const char *const kTypeNamesCN[TYPE_COUNT];",
              "",
              "#endif",
              ""]
    (GEN / "poke_types.h").write_text("\n".join(lines), encoding="utf-8")

    # 克制表：从已知 Gen3 关系硬编码构建（Veekun 的 damage_relations 在 type_names 无此字段）
    idx = {name: i for i, (tid, name) in enumerate(type_list)}

    # Gen VI 克制关系（攻击方 -> {守方: 倍率}），含 Fairy 与钢/幽灵/恶关系。
    # 只列出 != 1.0 的关系，未列出的即 1.0。
    chart = {
        "Normal":{"Rock": .5, "Ghost": 0},
        "Fighting":{"Normal": 2, "Ice": 2, "Poison": .5, "Flying": .5, "Psychic": .5, "Bug": .5, "Rock": 2, "Ghost": 0, "Dark": 2, "Steel": 2, "Fairy": .5},
        # 飞行对草/毒都是 1.0（早期误写为 2.0 会让飞行系打草/毒打出假克制）
        "Flying":{"Fighting": 2, "Bug": 2, "Rock": .5, "Electric": .5, "Steel": .5},
        "Poison":{"Poison": .5, "Ground": .5, "Rock": .5, "Ghost": .5, "Steel": 0, "Fairy": 2},
        "Ground":{"Flying": 0, "Poison": 2, "Rock": 2, "Bug": .5, "Steel": 2, "Fire": 2, "Grass": .5, "Electric": 2},
        "Rock":{"Fighting": 2, "Flying": 2, "Ground": .5, "Bug": 2, "Steel": .5, "Fire": 2, "Ice": 2},
        "Bug":{"Fighting": .5, "Flying": .5, "Poison": .5, "Ghost": .5, "Dark": 2, "Steel": .5, "Fire": .5, "Grass": 2, "Psychic": 2},
        "Ghost":{"Normal": 0, "Psychic": 2, "Ghost": 2, "Dark": .5},
        "Dark":{"Fighting": .5, "Psychic": 2, "Ghost": 2, "Dark": .5, "Fairy": .5},
        "Steel":{"Flying": .5, "Poison": 0, "Rock": 2, "Bug": .5, "Steel": .5, "Fairy": 2, "Ice": 2, "Fire": .5},
        # 火打钢是 0.5x（钢抗火），早期误写为 2.0
        "Fire":{"Rock": .5, "Bug": 2, "Steel": .5, "Grass": 2, "Ice": 2, "Dragon": .5, "Fire": .5, "Water": .5},
        "Water":{"Ground": 2, "Rock": 2, "Fire": 2, "Water": .5, "Grass": 2, "Dragon": .5},
        "Grass":{"Flying": .5, "Poison": .5, "Ground": 2, "Rock": 2, "Bug": 2, "Steel": .5, "Fire": .5, "Water": 2, "Grass": .5, "Dragon": .5},
        "Electric":{"Flying": 2, "Ground": 0, "Water": 2, "Grass": .5, "Electric": .5, "Dragon": .5},
        # Gen VI 起超能力打妖精 2x
        "Psychic":{"Fighting": 2, "Poison": 2, "Psychic": .5, "Dark": 0, "Steel": .5, "Fairy": 2},
        "Ice":{"Flying": 2, "Ground": 2, "Water": .5, "Grass": 2, "Ice": .5, "Dragon": 2, "Steel": .5, "Fire": .5},
        "Dragon":{"Dragon": 2, "Steel": .5, "Fairy": 0},
        "Fairy":{"Fighting": 2, "Poison": .5, "Dragon": 2, "Dark": 2, "Steel": .5, "Fire": .5},
    }

    n = len(type_list)
    lines = [
        "/* 由 tools/gen_gen3_data.py 自动生成 */",
        '#include "poke_types.h"',
        "",
        f"const uint8_t kTypeEffect[TYPE_MATRIX_SIZE][TYPE_MATRIX_SIZE] = {{",
    ]
    for tid, name in type_list:
        row = [10] * n
        for target, mult in chart.get(name, {}).items():
            if target in idx:
                row[idx[target]] = int(round(mult * 10))
        lines.append("    { " + ", ".join(f"{v:2d}" for v in row) + f" }},  /* {name} */")
    lines += ["};", "", "const char *const kTypeNames[TYPE_COUNT] = {"]
    for tid, name in type_list:
        lines.append(f'    "{name}",')
    lines += ["};", ""]

    # 简体中文属性名（Veekun 只有繁体，转简体）
    lines += ["const char *const kTypeNamesCN[TYPE_COUNT] = {"]
    for tid, name in type_list:
        cn = zh_types.get(tid, "")
        cn = cn.replace('"', '\\"')
        lines.append(f'    "{cn}",  /* {name} */')
    lines += ["};", ""]
    (GEN / "poke_types.c").write_text("\n".join(lines), encoding="utf-8")


CATEGORY_NAMES = {
    0: "MCAT_DAMAGE",          # 纯伤害
    1: "MCAT_AILMENT",         # 纯异常（电磁波/催眠粉）
    2: "MCAT_NET_GOOD_STATS",  # 自身能力提升（剑舞/龙舞）
    3: "MCAT_HEAL",            # 回复（生蛋/自我再生）
    4: "MCAT_DAMAGE_AILMENT",  # 伤害 + 异常（火焰拳/毒针）
    5: "MCAT_SWAGGER",         # 混乱类
    6: "MCAT_DAMAGE_LOWER",    # 伤害 + 降对方能力
    7: "MCAT_DAMAGE_RAISE",    # 伤害 + 升自身能力
    8: "MCAT_DAMAGE_HEAL",     # 伤害 + 吸取（吸取拳/亿万吸取）
    9: "MCAT_OHKO",            # 一击必杀
    10: "MCAT_WHOLE_FIELD",    # 全场效果（顺风/戏法空间）
    11: "MCAT_FIELD",          # 场地效果
    12: "MCAT_FORCE_SWITCH",   # 强制换人（吼叫/旋风）
    13: "MCAT_UNIQUE",         # 特殊机制，需逐个实现
}

AILMENT_NAMES = {
    -1: "MAIL_UNKNOWN",   # Veekun 的 unknown，等价于无异常
    0: "MAIL_NONE",
    1: "MAIL_PARALYSIS",
    2: "MAIL_SLEEP",
    3: "MAIL_FREEZE",
    4: "MAIL_BURN",
    5: "MAIL_POISON",
    6: "MAIL_CONFUSION",
    7: "MAIL_INFATUATION",
    8: "MAIL_TRAP",
    9: "MAIL_NIGHTMARE",
    12: "MAIL_TORMENT",
    13: "MAIL_DISABLE",
    14: "MAIL_YAWN",
    15: "MAIL_HEAL_BLOCK",
    17: "MAIL_NO_TYPE_IMMUNITY",
    18: "MAIL_LEECH_SEED",
    19: "MAIL_EMBARGO",
    20: "MAIL_PERISH_SONG",
    21: "MAIL_INGRAIN",
    24: "MAIL_SILENCE",
    42: "MAIL_TAR_SHOT",
    43: "MAIL_PROTECT",
}

# 本地 stat 索引（与 Statsheet.ey 的 1..6 一致）
STAT_NAME = {1: "hp", 2: "atk", 3: "def", 4: "spa", 5: "spd", 6: "spe",
             7: "acc", 8: "eva"}

# 单个招式最多影响几项能力。实测 Veekun 三代范围内最大 5 项：
#   生长 Growth(#246)、原始之力(#318)、纯粹电力(#466)、破壳(#702) 等
#   同时改攻/防/特攻/特防/速度。定 5 刚好覆盖，不留余量——
#   留余量会让每个招式多背字节，354 个招式不是小数。
MOVE_MAX_STAT_CHANGES = 5


def emit_moves(moves):
    cats = sorted({m["category"] for m in moves.values()})
    ails = sorted({m["ailment"] for m in moves.values()})

    lines = [
        "/* 由 tools/gen_gen3_data.py 自动生成 */",
        "#ifndef POKE_MOVES_H",
        "#define POKE_MOVES_H",
        "",
        "#include <stdint.h>",
        '#include "poke_types.h"',
        "",
        "/* ---------- 招式大类（Veekun move_meta.meta_category_id） ---------- */",
        "typedef enum {",
    ]
    for c in cats:
        lines.append(f"    {CATEGORY_NAMES.get(c, 'MCAT_UNIQUE')},  /* {c} */")
    lines += [
        "    MCAT_COUNT",
        "} MoveCategory;",
        "",
        "/* ---------- 异常状态（Veekun move_meta.meta_ailment_id） ---------- */",
        "typedef enum {",
    ]
    for a in ails:
        lines.append(f"    {AILMENT_NAMES.get(a, 'MAIL_NONE')},  /* {a} */")
    lines += [
        "    MAIL_COUNT",
        "} MoveAilment;",
        "",
        "typedef enum { DMG_STATUS = 0, DMG_PHYSICAL = 1, DMG_SPECIAL = 2 } DamageClass;",
        "",
        "/* 能力变化：哪一项、变几级 */",
        "typedef struct { uint8_t stat; int8_t change; } StatChange;",
        "",
        f"#define MOVE_MAX_STAT_CHANGES {MOVE_MAX_STAT_CHANGES}",
        "",
        "typedef struct {",
        "    uint16_t id;",
        "    const char *name;",
        "    const char *name_cn;   /* 简体中文名，UI 显示用 */",
        "    uint8_t type;",
        "    uint8_t power;",
        "    uint8_t pp;",
        "    uint8_t accuracy;      /* 0=必中 */",
        "    int8_t  priority;       /* -6..+5 */",
        "    uint8_t damage_class;",
        "    uint8_t category;      /* MoveCategory */",
        "    uint8_t ailment;       /* MoveAilment */",
        "    uint8_t ailment_chance;/* 异常触发概率 %，0 = 不触发 */",
        "    uint8_t stat_chance;   /* 能力变化概率 %，0 = 必定触发 */",
        "    uint8_t n_stat_changes;",
        "    StatChange stat_changes[MOVE_MAX_STAT_CHANGES];",
        "    uint8_t min_hits;",
        "    uint8_t max_hits;",
        "    uint8_t heal;           /* 治疗/吸取，HP * heal / 100 */",
        "    uint8_t crit_rate;      /* 0=普通 1=高 */",
        "    uint8_t flinch_chance;",
        "    uint8_t drain;          /* 反伤比例 % */",
        "    uint8_t min_turns;      /* 蓄力/持续下限 */",
        "    uint8_t max_turns;      /* 持续上限 */",
        "    uint16_t effect_id;     /* Veekun 原始 effect_id，仅调试参考，勿用于逻辑 */",
        "} MoveDef;",
        "",
        f"#define MOVE_COUNT {max(moves) if moves else 0}",
        "",
        "/*",
        " * 索引约定：kMoves[i].id == i + 1，即**下标 = move_id - 1**。",
        " * kMoves[0] 不是占位，而是 id 1 (Pound)。",
        " *",
        " * 因此绝不要写 kMoves[move_id] —— 那是下一个招式（move_id=MOVE_COUNT",
        " * 时直接越界读）。一律用下面的 mv_get()。",
        " */",
        "extern const MoveDef kMoves[MOVE_COUNT];",
        "",
        "/*",
        " * 按 move_id 取招式。id 非法（0 或 > MOVE_COUNT）返回 NULL。",
        " * 校验逻辑收敛在这里，避免每个调用点各自记错偏移。",
        " */",
        "static inline const MoveDef *mv_get(uint16_t move_id)",
        "{",
        "    return (move_id >= 1 && move_id <= MOVE_COUNT) ? &kMoves[move_id - 1]",
        "                                                   : NULL;",
        "}",
        "",
        "#endif",
    ]
    (GEN / "poke_moves.h").write_text("\n".join(lines), encoding="utf-8")

    max_id = max(moves) if moves else 0
    lines = [
        "/* 由 tools/gen_gen3_data.py 自动生成 */",
        '#include "poke_moves.h"',
        "",
        f"const MoveDef kMoves[{max_id}] = {{",
    ]
    for mid in range(1, max_id + 1):
        m = moves.get(mid)
        if not m:
            lines.append(
                f'    /* {mid} 缺 */ {{ {mid}, NULL, TYPE_NORMAL, 0, 0, 0, 0, 0, '
                f'MCAT_DAMAGE, MAIL_NONE, 0, 0, 0, {{0,0}}, 1, 1, 0, 0, 0, 0, 0, 0, 0 }},'
            )
            continue
        sc = m["stat_changes"][:MOVE_MAX_STAT_CHANGES]
        # 用 {stat=0, change=0} 占位填满，逗号分隔，避免 f-string 里的
        # 花括号被当成格式字段解析（曾因此生成 {0,0}{0,0}... 缺逗号的坏代码）。
        items = [f"{{{st}, {ch}}}" for st, ch in sc]
        items += ["{0, 0}"] * (MOVE_MAX_STAT_CHANGES - len(items))
        parts = ", ".join(items)
        cn_mv = (zh_moves.get(mid, "") or "").replace('"', '\\"')
        lines.append(
            f'    {{ {mid}, "{m["name"]}", "{cn_mv}", TYPE_{m["type_name"]}, '
            f'{m["power"]}, {m["pp"]}, {m["accuracy"]}, {m["priority"]}, '
            f'DMG_{["STATUS","PHYSICAL","SPECIAL"][m["damage_class"]]}, '
            f'{CATEGORY_NAMES.get(m["category"], "MCAT_UNIQUE")}, '
            f'{AILMENT_NAMES.get(m["ailment"], "MAIL_NONE")}, '
            f'{m["ailment_chance"]}, {m["stat_chance"]}, {len(sc)}, '
            f'{{ {parts} }}, '
            f'{m["min_hits"]}, {m["max_hits"]}, {m["healing"]}, {m["crit_rate"]}, '
            f'{m["flinch_chance"]}, {m["drain"]}, {m["min_turns"]}, {m["max_turns"]}, '
            f'{m["effect_id"]} }},'
        )
    lines += ["};", ""]
    (GEN / "poke_moves.c").write_text("\n".join(lines), encoding="utf-8")


def emit_species(species, mega_map):
    lines = [
        "/* 由 tools/gen_gen3_data.py 自动生成 */",
        "#ifndef POKE_SPECIES_H",
        "#define POKE_SPECIES_H",
        "",
        "#include <stdint.h>",
        '#include "poke_types.h"',
        "",
        "typedef struct {",
        "    uint16_t id;        /* pokemon id，1..386，Mega 段 10034..10134 */",
        "    uint16_t species_id;",
        "    const char *name;",
        "    const char *name_cn;   /* 简体中文名，UI 显示用 */",
        "    uint8_t gen;         /* 1..3 */",
        "    uint8_t is_mega;",
        "    uint8_t type1, type2;",
        "    uint8_t hp, atk, def, spa, spd, spe;   /* 种族值 */",
        "    uint8_t catch_rate;",
        "    uint8_t growth_rate;",
        "    int8_t  gender_rate;",
        "} SpeciesDef;",
        "",
        f"#define SPECIES_COUNT {len(species)}",
        "extern const SpeciesDef kSpecies[SPECIES_COUNT];",
        "",
        "/* Mega -> 基础形态 species_id 映射表 */",
        f"#define MEGA_COUNT {len(mega_map)}",
        "typedef struct { uint16_t mega_pid; uint16_t base_species_id; } MegaMap;",
        "extern const MegaMap kMegaMap[MEGA_COUNT];",
        "",
        "const SpeciesDef *species_get(uint16_t pokemon_id);",
        "uint16_t mega_to_base(uint16_t pokemon_id);",
        "",
        "#endif",
    ]
    (GEN / "poke_species.h").write_text("\n".join(lines), encoding="utf-8")

    lines = [
        "/* 由 tools/gen_gen3_data.py 自动生成 */",
        '#include "poke_species.h"',
        "",
        "const SpeciesDef kSpecies[SPECIES_COUNT] = {",
    ]
    for s in species:
        t1 = f"TYPE_{s['type1_name']}" if s["type1_name"] else "TYPE_NORMAL"
        t2 = f"TYPE_{s['type2_name']}" if s["type2_name"] else t1
        cn = (zh_species.get(s["species_id"], "") or "").replace('"', '\\"')
        lines.append(
            f'    /* #{s["id"]} */\n'
            f'    {{ {s["id"]}, {s["species_id"]}, "{s["ident"]}", "{cn}", {s["gen"]}, {s["is_mega"]}, '
            f"{t1}, {t2}, {s['hp']}, {s['atk']}, {s['def']}, {s['spa']}, {s['spd']}, {s['spe']}, "
            f'{s["catch_rate"]}, {s["growth"]}, {s["gender_rate"]} }},'
        )
    lines += ["};", ""]
    lines += [f"const MegaMap kMegaMap[{len(mega_map)}] = {{"]
    for mega_pid, base_sid in sorted(mega_map.items()):
        lines.append(f"    {{ {mega_pid}, {base_sid} }},")
    lines += ["};", ""]
    lines += [
        "const SpeciesDef *species_get(uint16_t pokemon_id)",
        "{",
        "    for (unsigned i = 0; i < SPECIES_COUNT; i++) {",
        "        if (kSpecies[i].id == pokemon_id) return &kSpecies[i];",
        "    }",
        "    return NULL;",
        "}",
        "",
        "uint16_t mega_to_base(uint16_t pokemon_id)",
        "{",
        "    for (unsigned i = 0; i < MEGA_COUNT; i++) {",
        "        if (kMegaMap[i].mega_pid == pokemon_id) return kMegaMap[i].base_species_id;",
        "    }",
        "    return pokemon_id;",
        "}",
        "",
    ]
    (GEN / "poke_species.c").write_text("\n".join(lines), encoding="utf-8")


def emit_learnsets(learnsets, valid_pids):
    MAXN = 24
    lines = [
        "/* 由 tools/gen_gen3_data.py 自动生成 */",
        "#ifndef POKE_LEARNSETS_H",
        "#define POKE_LEARNSETS_H",
        "",
        "#include <stdint.h>",
        "",
        f"#define LEARNSET_MAX_NODES {MAXN}",
        "typedef struct { uint8_t level; uint16_t move_id; } LearnEntry;",
        "",
        "typedef struct {",
        "    uint8_t node_count;",
        "    LearnEntry nodes[LEARNSET_MAX_NODES];",
        "} LearnSet;",
        "",
        "/* 检索用行结构：pid + 该 pid 的学习表 */",
        "typedef struct { uint16_t pid; LearnSet set; } LearnSetRow;",
        "",
        f"#define LEARNSET_COUNT {len(valid_pids)}",
        "extern const LearnSetRow kLearnSets[LEARNSET_COUNT];",
        "",
        "/* 按 pokemon_id 查学习表，找不到返回 NULL */",
        "const LearnSet *learnset_get(uint16_t pokemon_id);",
        "",
        "#endif",
    ]
    (GEN / "poke_learnsets.h").write_text("\n".join(lines), encoding="utf-8")

    lines = [
        "/* 由 tools/gen_gen3_data.py 自动生成 */",
        '#include "poke_learnsets.h"',
        "",
        "/* 每个条目带 pokemon_id，便于 learnset_get 查找 */",
        f"const LearnSetRow kLearnSets[{len(valid_pids)}] = {{",
    ]
    for pid in valid_pids:
        nodes = learnsets.get(pid, [])[:MAXN]
        parts = [f"{{{lv}, {mid}}}" for mid, lv in nodes]
        lines.append(
            f"    {{ {pid}, {{ {len(nodes)}, {{ {', '.join(parts)}}} }} }},"
        )
    lines += ["};", ""]
    lines += [
        "const LearnSet *learnset_get(uint16_t pokemon_id)",
        "{",
        "    for (unsigned i = 0; i < LEARNSET_COUNT; i++) {",
        "        if (kLearnSets[i].pid == pokemon_id) return &kLearnSets[i].set;",
        "    }",
        "    return NULL;",
        "}",
        "",
    ]
    (GEN / "poke_learnsets.c").write_text("\n".join(lines), encoding="utf-8")


def emit_abilities(abilities):
    """
    特性表：枚举由数据驱动生成，不再手写清单。

    曾经这里硬编码了一份 ABIL_* 枚举清单，结果两个问题：
      1) 清单和 Veekun 的 ability id 对不上，且出现 ABIL_LEAF_GUARD_2 /
         ABIL_SLOW_START_4 这种为了去重而后加的脏后缀
      2) .c 里每条记录的 kind 全填 ABIL_NONE，等于没接上
    现在按 ability id 顺序生成 ABIL_<IDENT_UPPER>，kind 直接填真值。

    kind 的**效果语义**不在 Veekun 数据里（ability_effect_text 表 404，
    ability_changelog 只有 id 对照），由 src/poke_ability.c 手写实现。
    """
    lines = [
        "/* 由 tools/gen_gen3_data.py 自动生成 */",
        "#ifndef POKE_ABILITIES_H",
        "#define POKE_ABILITIES_H",
        "",
        "#include <stdint.h>",
        "",
        "/*",
        " * 特性枚举。命名规则 ABIL_<英文名大写>，顺序与 kAbilities 一致。",
        " * 效果实现见 src/poke_ability.c（Veekun 无机读效果数据，需手写）。",
        " */",
        "typedef enum {",
        "    ABIL_NONE = 0,",
    ]
    for a in abilities:
        lines.append(f"    ABIL_{a['name'].upper()},")
    lines += [
        "    ABIL_COUNT",
        "} AbilityKind;",
        "",
        "typedef struct { uint16_t id; uint8_t kind; const char *name; "
        "const char *name_cn; } AbilityDef;",
        "",
        f"#define ABILITY_COUNT {len(abilities)}",
        "extern const AbilityDef kAbilities[ABILITY_COUNT];",
        "",
        "/* 按 Veekun ability id 查特性，找不到返回 NULL */",
        "const AbilityDef *ability_get(uint16_t ability_id);",
        "",
        "#endif",
    ]
    (GEN / "poke_abilities.h").write_text("\n".join(lines), encoding="utf-8")

    lines = [
        "/* 由 tools/gen_gen3_data.py 自动生成 */",
        '#include "poke_abilities.h"',
        "",
        f"const AbilityDef kAbilities[ABILITY_COUNT] = {{",
    ]
    for a in abilities:
        cn_ab = (zh_abilities.get(a["id"], "") or "").replace('"', '\\"')
        lines.append(f'    {{ {a["id"]}, ABIL_{a["name"].upper()}, '
                     f'"{a["name"]}", "{cn_ab}" }},')
    lines += [
        "};",
        "",
        "const AbilityDef *ability_get(uint16_t ability_id)",
        "{",
        "    for (unsigned i = 0; i < ABILITY_COUNT; i++) {",
        "        if (kAbilities[i].id == ability_id) return &kAbilities[i];",
        "    }",
        "    return NULL;",
        "}",
        "",
    ]
    (GEN / "poke_abilities.c").write_text("\n".join(lines), encoding="utf-8")


def emit_natures():
    # Nature: id -> (increased_stat, decreased_stat)，0=无修正
    # stat 顺序: hp(1) atk(2) def(3) spa(4) spd(5) spe(6)
    NATURES = {
        1:("hardy",0,0), 2:("lonely",2,3), 3:("brave",2,6), 4:("adamant",2,5),
        5:("naughty",2,4), 6:("bold",3,2), 7:("docile",0,0), 8:("relaxed",3,6),
        9:("impish",3,5), 10:("lax",3,4), 11:("timid",6,2), 12:("hasty",6,3),
        13:("serious",0,0), 14:("jolly",6,4), 15:("naive",6,5), 16:("modest",4,2),
        17:("mild",4,3), 18:("quiet",4,6), 19:("bashful",0,0), 20:("rash",4,5),
        21:("calm",5,2), 22:("gentle",5,3), 23:("sassy",5,6), 24:("careful",5,4),
        25:("quirky",0,0),
    }
    lines = [
        "/* 由 tools/gen_gen3_data.py 自动生成 */",
        "#ifndef POKE_NATURES_H",
        "#define POKE_NATURES_H",
        "",
        "#include <stdint.h>",
        "",
        "typedef struct {",
        "    uint8_t increased_stat;  /* 0=无, 1=hp 2=atk 3=def 4=spa 5=spd 6=spe */",
        "    uint8_t decreased_stat;",
        "    const char *name;",
        "    const char *name_cn;   /* 简体中文名，UI 显示用 */",
        "} NatureDef;",
        "",
        "#define NATURE_COUNT 26",
        "extern const NatureDef kNatures[NATURE_COUNT];",
        "",
        "#endif",
    ]
    (GEN / "poke_natures.h").write_text("\n".join(lines), encoding="utf-8")

    lines = [
        "/* 由 tools/gen_gen3_data.py 自动生成 */",
        '#include "poke_natures.h"',
        "",
        "const NatureDef kNatures[NATURE_COUNT] = {",
    ]
    # kNatures[0] 是「无性格」占位，索引 = nature_id，
    # 所以第1 项必须占住 index 0（曾漏掉这行导致整表错位一格）。
    lines.append('    { 0, 0, "neutral", "无"},  /* nature 0 */')
    for nid in range(1, 26):
        name, inc, dec = NATURES[nid]
        cn = zh_natures.get(nid, "").replace('"', '\\"')
        lines.append(f'    {{ {inc}, {dec}, "{name}", "{cn}" }},'
                     f'  /* nature {nid} */')
    lines += ["};", ""]
    (GEN / "poke_natures.c").write_text("\n".join(lines), encoding="utf-8")


# ============================================================
# 携带道具效果表（手写）
# ============================================================
# Veekun 没有机读的道具效果数据：
#   item_effects.csv        404 不存在
#   item_fling_effects.csv  只有 7 种 fling 效果（与携带无关）
#   item_flavor_text.csv    只覆盖球类道具（item_id 1~18），携带物没有描述
# 所以这张表和特性一样是手写的，数值按官方规则。
#
# 字段含义见 poke_items.h 的 ItemDef。
# 键 = Veekun item id，值 = dict(同名字段)。
#
# 三代图鉴内常用的携带物都在这里；未列出的携带物视为"只有类别、无效果"
# （如力量负重等减肥道具，对战斗无影响，可以不实现）。
ITEM_EFFECTS = {
    # ---- 属性威力增强类（+20%，第六世代从 +10% 上调到 +20%）----
    226: {"boost_type": "Fire",   "boost_amount": 20},    # 木炭
    216: {"boost_type": "Grass",  "boost_amount": 20},    # 奇迹种子
    220: {"boost_type": "Water",  "boost_amount": 20},    # 神秘水滴
    215: {"boost_type": "Rock",   "boost_amount": 20},    # 硬石头
    219: {"boost_type": "Electric","boost_amount": 20},   # 磁铁
    223: {"boost_type": "Ice",    "boost_amount": 20},    # 不融冰
    218: {"boost_type": "Fighting","boost_amount": 20},   # 黑带
    222: {"boost_type": "Poison", "boost_amount": 20},    # 毒针
    214: {"boost_type": "Ground", "boost_amount": 20},    # 柔软沙子
    221: {"boost_type": "Flying", "boost_amount": 20},    # 锐利鸟嘴
    225: {"boost_type": "Psychic","boost_amount": 20},    # 弯曲勺子
    227: {"boost_type": "Dragon", "boost_amount": 20},    # 龙之牙
    228: {"boost_type": "Normal", "boost_amount": 20},    # 丝绸围巾
    217: {"boost_type": "Bug",    "boost_amount": 20},    # 银粉
    224: {"boost_type": "Ghost",  "boost_amount": 20},    # 诅咒符
    229: {"boost_type": "Dark",   "boost_amount": 20},    # 黑色眼镜
    231: {"boost_type": "Steel",  "boost_amount": 20},    # 金属涂层

    # ---- 能力值增强类 ----
    235: {"boost_stat": 2, "stat_amount": 100},   # 粗骨头：卡比兽攻击翻倍
    213: {"boost_stat": 4, "stat_amount": 100},   # 电气球：皮卡丘特攻翻倍
    233: {"boost_stat": 2, "stat_amount": 100},   # 幸运拳套：+-攻击翻倍
    204: {"boost_stat": 5, "stat_amount": 100},   # 深海鳞片：珍珠贝特防翻倍
    234: {"boost_stat": 3, "stat_amount": 100},   # 金属粉末：-防御翻倍

    # ---- 会心与畏缩 ----
    209: {"crit_stage": 1},               # 焦点镜片：会心率提升一级
    207: {"focus_pct": 30},               # 专注剑（不在三代，占位）
    190: {"immune_flinch": 1},            # 光亮粉末：免疫畏缩
    194: {"quick_pct": 20},               # 快捷腰带：20% 先手
    211: {"leftovers_pct": 16},           # 吃剩的东西：每回合末回 1/16
    230: {"shell_bell_pct": 8},           # 贝壳之铃：造成伤害回 1/8
    265: {"chip_pct": 12},                # 粘性钩：反伤

    # ---- 状态/能力免疫 ----
    208: {"immune_stat_drop": 1},         # 气势头带：防能力下降（近似）
    236: {"immune_stat_drop": 1},         # 王者信物：防畏缩与下降（近似）

    # ---- 树果（回复量按 HP 比例或固定值）----
    # 三代树果回复 30 点（文柚果等特殊树果另有状态效果，未实现）
    # 具体 heal 值由 emit_items 按 BERRY_HEAL 统一填。
}

# 三代树果统一回复量。文柚果/花椰果等有附加状态效果的树果
# 在本项目里只做回血，附加效果未实现（对 AI 评估影响有限）。
BERRY_HEAL = 30

def emit_items(items):
    """道具表不导出名称字符串——名称只在 PC 端配置界面用，
    固件侧靠 ID 判断类型（树果/携带物/Mega 石），可省下大量 Flash。"""
    lines = [
        "/* 由 tools/gen_gen3_data.py 自动生成 */",
        "#ifndef POKE_ITEMS_H",
        "#define POKE_ITEMS_H",
        "",
        "#include <stdint.h>",
        "",
        "typedef enum {",
        "    ITEMKIND_NONE = 0,",
        "    ITEMKIND_BERRY,",
        "    ITEMKIND_HOLD,",
        "    ITEMKIND_MEGA_STONE,",
        "    ITEMKIND_TYPE_SPECIFIC,",
        "    ITEMKIND_RESTORE,",
        "} ItemKind;",
        "",
        "typedef struct {",
        "    uint16_t id;",
        "    const char *name;",
        "    const char *name_cn;   /* 简体中文名，UI 显示用 */",
        "    uint8_t  kind;",
        "    /* ---- 携带时的常驻效果 ---- */",
        "    uint16_t heal;        /* 树果类：回复量 HP */",
        "    uint8_t  boost_type; /* 增强某属性招式威力，0 = 无 */",
        "    uint8_t  boost_amount;/* 增强百分比，20 = +20% */",
        "    uint8_t  boost_stat; /* 增强某能力值，0=无 2=atk 3=def 4=spa 5=spd 6=spe */",
        "    int8_t   boost_turns;",
        "    uint8_t  stat_amount;/* 能力增强百分比 */",
        "    /* ---- 概率与特殊效果 ---- */",
        "    uint8_t  flinch_pct;  /* 携带者攻击时使对手畏缩的概率 */",
        "    uint8_t  crit_stage;  /* 额外暴击等级 */",
        "    uint8_t  focus_pct;   /* 焦点镜片类：会心率提升 */",
        "    uint8_t  quick_pct;   /* 快捷腰带给先手概率 */",
        "    uint8_t  leftovers_pct; /* 吃剩的东西：回合末回血百分比 */",
        "    uint8_t  shell_bell_pct; /* 贝壳之铃：造成伤害回血百分比 */",
        "    uint8_t  chip_pct;    /* 钢妖反伤（锐利之牙/铁刺）百分比 */",
        "    /* ---- 状态免疫 ---- */",
        "    uint8_t  immune_stat_drop; /* 1 = 防能力下降 */",
        "    uint8_t  immune_crit;      /* 1 = 防会心一击 */",
        "    uint8_t  immune_flinch;    /* 1 = 防畏缩 */",
        "} ItemDef;",
        "",
        f"#define ITEM_COUNT {len(items)}",
        "extern const ItemDef kItems[ITEM_COUNT];",
        "",
        "/* 按道具 id 查定义，找不到返回 NULL */",
        "const ItemDef *item_get(uint16_t item_id);",
        "",
        "#endif",
    ]
    (GEN / "poke_items.h").write_text("\n".join(lines), encoding="utf-8")

    # 用道具英文名推断类别。Veekun 无 category 字段，故按命名判断。
    # Mega 石的官方命名是 "<基础种名>ite"（Charizardite / Venusaurite），
    # 而 Mega Ring / Mega Glove 等携带物不含 "ite"，据此区分。
    def classify(name):
        n = name.lower().strip()
        # Mega 石：靠精确白名单判断，不能只看 "ite" 后缀。
        # 名字以 ite 结尾的非 Mega 道具实测有：
        #   Meteorite（陨石，id 537）、Black Augurite、Sablenite、
        #   Meteorite Shard 等，用后缀匹配会把它们全收进来。
        if name in MEGA_STONE_NAMES:
            return "ITEMKIND_MEGA_STONE"
        # 注意 Leftovers（吃剩的东西）不是树果，是携带物，
        # 它的效果在 ITEM_EFFECTS 里用 leftovers_pct 表达。
        if "berry" in n:
            return "ITEMKIND_BERRY"
        if any(k in n for k in ("potion", "elixir", "drop", "balm", "mix", "soda",
                                 "punch", "herbal", "revive", "full restore")):
            return "ITEMKIND_RESTORE"
        if any(k in n for k in ("band", "specs", "vest", "boots", "belt", "bracer",
                                 "lens", "bandage", "incense", "weight", "throat",
                                 "anchor", "choice", "type")):
            return "ITEMKIND_TYPE_SPECIFIC"
        return "ITEMKIND_HOLD"

    lines = [
        "/* 由 tools/gen_gen3_data.py 自动生成 */",
        '#include "poke_items.h"',
        "",
        f"const ItemDef kItems[{len(items)}] = {{",
    ]
    for it in items:
        kind = classify(it["name"])
        eff = ITEM_EFFECTS.get(it["id"], {})
        # 树果统一回复量（ITEN_EFFECTS 里没单独列每棵树果）
        heal = it["heal"]
        if kind == "ITEMKIND_BERRY" and heal == 0:
            heal = BERRY_HEAL
        bt = eff.get("boost_type", "None")
        # boost_type 用属性名，需转成 TYPE_xxx 枚举
        type_map = {n: i for i, n in enumerate(
            ["Normal", "Fighting", "Flying", "Poison", "Ground", "Rock", "Bug",
             "Ghost", "Steel", "Fire", "Water", "Grass", "Electric", "Psychic",
             "Ice", "Dragon", "Dark", "Fairy"])}
        bt_idx = type_map.get(bt, 0) if bt != "None" else 0
        cn_it = (zh_items.get(it["id"], "") or "").replace('"', '\\"')
        lines.append(
            f'    {{ {it["id"]}, "{it["name"]}", "{cn_it}", {kind}, {heal}, '
            f'{bt_idx}, {eff.get("boost_amount", 0)}, '
            f'{eff.get("boost_stat", 0)}, {eff.get("boost_turns", 0)}, '
            f'{eff.get("stat_amount", 0)}, '
            f'{eff.get("flinch_pct", 0)}, {eff.get("crit_stage", 0)}, '
            f'{eff.get("focus_pct", 0)}, {eff.get("quick_pct", 0)}, '
            f'{eff.get("leftovers_pct", 0)}, {eff.get("shell_bell_pct", 0)}, '
            f'{eff.get("chip_pct", 0)}, '
            f'{1 if eff.get("immune_stat_drop") else 0}, '
            f'{1 if eff.get("immune_crit") else 0}, '
            f'{1 if eff.get("immune_flinch") else 0} }},  /* {it["name"]} */'
        )
    lines += ["};", ""]
    lines += [
        "const ItemDef *item_get(uint16_t item_id)",
        "{",
        "    for (unsigned i = 0; i < ITEM_COUNT; i++) {",
        "        if (kItems[i].id == item_id) return &kItems[i];",
        "    }",
        "    return NULL;",
        "}",
        "",
    ]
    (GEN / "poke_items.c").write_text("\n".join(lines), encoding="utf-8")


if __name__ == "__main__":
    main()