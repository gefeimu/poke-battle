"""
zh_names.py - 中文名提取与繁简转换

数据源
------
Veekun 的 *_names.csv 每种名字对多种语言各有一行：
    language_id=9  英文（现在数据表里存的那份，用于代码内标识）
    language_id=4  **繁体中文**

注意 Veekun 没有简体行（简体与繁体的英文名相同，没有单独的语言标记），
所以这里取繁体再转简体。转换用Windows 自带的 LCMapStringEx，
不需要引入 opencc / zhconv 之类的依赖——本项目要能在一台裸 Windows
上重建全部数据表，装第三方包会带来不必要的失败点。

为什么不直接把中文名放进数据表
------------------------------
数据表现在存的 `name` 是英文标识符（`venusaur`），它同时是代码里的
查找键（测试里的 `find_move("Flamethrower")` 依赖它），不能替换。
所以新增 `name_cn` 字段，两份并存：英文给代码用，中文给 UI 显示。
"""

import csv
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
CSV = ROOT / "data" / "csv"

# Veekun 的繁体中文语言 id
LANG_ZH_HANT = 4
LANG_ZH_HANS = 4  # 数据源只有繁体，转换后得到简体

# 各实体对应的 names 表与 id 列
NAME_TABLES = {
    "species": ("pokemon_species_names.csv", "pokemon_species_id"),
    "moves": ("move_names.csv", "move_id"),
    "types": ("type_names.csv", "type_id"),
    "abilities": ("ability_names.csv", "ability_id"),
    "items": ("item_names.csv", "item_id"),
    "natures": ("nature_names.csv", "nature_id"),
}


# ---------------------------------------------------------------- 繁简转换

def _init_lcmap():
    """绑定 Windows LCMapStringEx。返回函数或None（非 Windows 时）。"""
    import ctypes
    from ctypes import wintypes

    try:
        k32 = ctypes.windll.kernel32
    except (AttributeError, OSError):
        return None

    fn = k32.LCMapStringEx
    fn.argtypes = [
        wintypes.LPCWSTR, wintypes.DWORD, wintypes.LPCWSTR, ctypes.c_int,
        wintypes.LPWSTR, ctypes.c_int, ctypes.c_void_p, ctypes.c_void_p,
        wintypes.LPARAM,
    ]
    fn.restype = ctypes.c_int
    return fn


_LCMAP = _init_lcmap()
# LCMAP_SIMPLIFIED_CHINESE = 0x02000000
_SIMPLIFIED = 0x02000000

# Windows 的繁简转换表在这几个生僻异体字上有缺口：常用字（種→种、
# 噴→喷、鉗→钳）都正常，但下面这些**原样返回**。实测方式是把项目六个
# 数据表里的繁体名全跑一遍，挑出转换后仍含异体字的条目。
#
# 补表只在 Windows 转换**没有改动**该字符时才生效，不会覆盖正常转换。
T2S_SUPPLEMENT = {
    "癒": "愈",   # 治癒鈴聲 -> 治愈铃声（治愈，非治癒）
    "蠍": "蝎",   # 天蠍 -> 天蝎；鉗尾蠍 -> 钳尾蝎
    "闇": "暗",   # 黑闇石 -> 黑暗石
    "勳": "勋",   # 卡洛斯勳章 -> 卡洛斯勋章
    "燻": "熏",   # 煙燻尾巴 -> 烟熏尾巴
    "荳": "豆",   # 伽勒荳蔻枝 -> 伽勒豆蔻枝
}

# 注意「糬」不在此表：麻糬的「糬」简体也写作「糬」，没有对应简体字。
# 它在得意黑里缺字形，属于字体覆盖问题而非转换问题。


def to_simplified(text):
    """繁体转简体。转换不可用时原样返回（会保留繁体字）。"""
    if not text:
        return text
    if _LCMAP is not None:
        import ctypes

        n = _LCMAP("zh-CN", _SIMPLIFIED, text, len(text), None, 0, None, None, 0)
        if n > 0:
            buf = ctypes.create_unicode_buffer(n)
            _LCMAP("zh-CN", _SIMPLIFIED, text, len(text), buf, n, None, None, 0)
            text = buf[:n]
    # 补 Windows 漏掉的生僻字
    return "".join(T2S_SUPPLEMENT.get(c, c) for c in text)


# ---------------------------------------------------------------- 读取

_cache = {}


def load_names(kind):
    """
    读某类实体的简体中文名。返回 {id: 简体名}。

    kind 取 NAME_TABLES 的键。文件缺失时返回空 dict（不抛异常）——
    数据表生成不该因为缺中文名就整个失败，但调用方要能发现结果是空的。
    """
    if kind in _cache:
        return _cache[kind]

    fname, id_col = NAME_TABLES[kind]
    path = CSV / fname
    out = {}
    if path.exists():
        with open(path, encoding="utf-8") as f:
            for r in csv.DictReader(f):
                if r.get("local_language_id") == str(LANG_ZH_HANT):
                    raw = r["name"]
                    # 名字里的空格是官方的排版空格，UI 里会自己排版
                    out[int(r[id_col])] = to_simplified(raw).strip()
    _cache[kind] = out
    return out


def load_all():
    """一次性拿全部六类中文名。"""
    return {kind: load_names(kind) for kind in NAME_TABLES}


def collect_chars():
    """
    收集所有中文名里出现过的字符。

    这是字库子集化的输入——比「扫C 文件里的字符串字面量」大得多，
    因为数据表的中文名是以结构体字段存放的，扫源码扫不到。
    """
    chars = set()
    for names in load_all().values():
        for name in names.values():
            chars.update(c for c in name if c.isprintable() and not c.isspace())
    return chars


if __name__ == "__main__":
    names = load_all()
    print("=== 中文名覆盖 ===")
    for kind, m in names.items():
        print(f"  {kind:10s} {len(m):5d} 条")

    cn = load_names("species")
    print("\n=== 三代前 12 只宝可梦 ===")
    for i in range(1, 13):
        print(f"  {i:3d} {cn.get(i, '（缺）')}")

    mv = load_names("moves")
    print("\n=== 常用招式 ===")
    for i in [1, 53, 394, 15]:
        print(f"  {i:4d} {mv.get(i, '（缺）')}")

    ty = load_names("types")
    print("\n=== 18 属性 ===")
    print("  " + " ".join(ty.get(i, "?") for i in range(1, 19)))

    chars = collect_chars()
    print(f"\n去重字符数: {len(chars)}")
    hanzi = [c for c in chars if ord(c) > 0x2E80]
    print(f"  其中汉字: {len(hanzi)}")
    print(f"  其他(标点/符号): {len(chars) - len(hanzi)}")