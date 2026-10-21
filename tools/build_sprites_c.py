"""
从本地 sparse checkout 目录生成精灵图 4bpp C 数组。

输入（已由用户 sparse checkout 到本地）
    <src>/generation-iii/ruby-sapphire/{id}.png        64x64 正面，386 张
    <src>/generation-iii/ruby-sapphire/back/{id}.png   64x64 背面，386 张
    <src>/generation-iii/ruby-sapphire/shiny/{id}.png  64x64 闪亮，386 张
    <src>/generation-v/black-white/{mega_pid}.png96x96 Mega 立绘，52 张

处理
    1. 冗余清理：只保留图鉴号 1..386 的正面/背面/闪亮，
       Mega 段（pid>=10000）里只保留 kMegaMap 中的 52 个，
       其余 Deoxys 形态 / Castform 天气形态 / 四代以上全丢掉
    2. 量化到 16 色（4bpp），透明像素阈值化为索引 0
    3. 打包 4bpp：每字节 2 像素（4 bit/像素），高位在前，行优先
    4. 调色板导成 RGB565 数组

输出
    data/sprites_raw/      精简后的原图（可删，可重新生成）
    data/sprites_4bpp/     量化后的 PNG-8 中间产物（便于检查画质）
    gen/poke_sprites.c/.h  固件用的 C 数组

用法
    python tools/build_sprites_c.py \
        --src D:/Download/Chromedownload/sprites/sprites/pokemon/versions
"""

import argparse
import re
import shutil
import sys
from pathlib import Path

from PIL import Image

ROOT = Path(__file__).resolve().parent.parent
RAW = ROOT / "data" / "sprites_raw"
Q4 = ROOT / "data" / "sprites_4bpp"
GEN = ROOT / "gen"

GEN3_MAX = 386
PALETTE_COLORS = 16
TRANSPARENT_INDEX = 0
ICON_SIZE = 64      # 三代原生尺寸，无需缩放
MEGA_SIZE = 64      # Mega 立绘 96 -> 64


# ---------------------------------------------------------------- 读取清单

def load_species_meta():
    """从 gen/poke_species.c 读出 (图鉴号 -> (是否Mega, ident))。"""
    src = GEN / "poke_species.c"
    if not src.exists():
        raise SystemExit("缺少 gen/poke_species.c，请先运行 tools/gen_gen3_data.py")
    text = src.read_text(encoding="utf-8")
    normal, mega = {}, {}
    for pid, sid, ident, gen, is_mega in re.findall(
        r'\{ (\d+), (\d+), "([\w-]+)", (\d+), (\d),', text
    ):
        if is_mega == "1":
            mega[int(pid)] = (int(sid), ident)
        else:
            normal[int(pid)] = ident
    return normal, mega


# ---------------------------------------------------------------- 图像处理

def trim_alpha(im):
    """裁掉全透明边缘。不裁的话缩放后精灵会偏在一边。"""
    im = im.convert("RGBA")
    bbox = im.getbbox()
    return im.crop(bbox) if bbox else im


def fit_size(im, size):
    """等比缩放到 size 画布并居中，留 1px 边距。"""
    t = trim_alpha(im)
    inner = size - 2
    ratio = min(inner / t.width, inner / t.height)
    w = max(1, round(t.width * ratio))
    h = max(1, round(t.height * ratio))
    scaled = t.resize((w, h), Image.LANCZOS)
    canvas = Image.new("RGBA", (size, size), (0, 0, 0, 0))
    canvas.alpha_composite(scaled, ((size - w) // 2, (size - h) // 2))
    return canvas


def quantize16(im):
    """量化到 16 色，返回 (索引图, 硬alpha)。"""
    im = im.convert("RGBA")
    alpha = im.getchannel("A").point(lambda a: 255 if a >= 128 else 0)
    rgb = Image.new("RGB", im.size, (0, 0, 0))
    rgb.paste(im.convert("RGB"), mask=alpha)
    q = rgb.quantize(colors=PALETTE_COLORS, method=Image.Quantize.MEDIANCUT)
    return q, alpha


def pack_4bpp(q, alpha):
    """打包 4bpp：每字节 2 像素，高位在前。像素 i 的索引
    = (data[i/2] >> ((i%2) ? 0 : 4)) & 0xF。

    注意这里曾是 `(v & 0x03) << (6 - k*2)`，那只保留低 2 bit、
    每字节塞 4 像素 —— 也就是真正的 2bpp。而调色板是按 16 色
    生成的，两边不一致的结果是：量化好的 16 色图被砍到只剩 4 色
    （实测索引 1 只占193/4096 像素），精灵图变成无法辨认的碎噪点。
    """
    w, h = q.size
    ip = q.load()
    ap = alpha.load()
    out = bytearray()
    total = w * h
    for i in range(0, total, 2):
        b = 0
        for k in range(2):
            j = i + k
            if j >= total:
                v = TRANSPARENT_INDEX
            else:
                x, y = j % w, j // w
                v = TRANSPARENT_INDEX if ap[x, y] == 0 else ip[x, y]
            b |= (v & 0x0F) << (4 - k * 4)
        out.append(b)
    return bytes(out)


def palette565(q):
    """导调色板为 RGB565 数组，索引 0 强制置黑（透明占位）。"""
    pal = q.getpalette() or []
    out = []
    for i in range(PALETTE_COLORS):
        if i * 3 + 2 < len(pal):
            r, g, b = pal[i * 3], pal[i * 3 + 1], pal[i * 3 + 2]
        else:
            r = g = b = 0
        if i == TRANSPARENT_INDEX:
            r = g = b = 0
        out.append(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3))
    return out


def save_preview(q, alpha, path):
    """存PNG-8 中间产物。必须用 P 模式：原图就是 PNG-8，
    存 RGBA 会让每像素从 1 字节涨到 4 字节。"""
    idx = q.copy()
    px = idx.load()
    a = alpha.load()
    w, h = idx.size
    for y in range(h):
        for x in range(w):
            if a[x, y] == 0:
                px[x, y] = TRANSPARENT_INDEX
    pal = list((idx.getpalette() or [])[:255 * 3]) or []
    while len(pal) < 256 * 3:
        pal += [0, 0, 0]
    idx.putpalette(pal)
    idx.info["transparency"] = TRANSPARENT_INDEX
    idx.save(path, optimize=True)


# ---------------------------------------------------------------- 收集与清理

def collect(src, normal_map, mega_map, gen3_dir="firered-leafgreen",
            keep_shiny=False, keep_back=False):
    """扫描源目录，收集需要的图，同时报告冗余。

    gen3_dir 可选：
      ruby-sapphire     红宝石/蓝宝石
      emerald           绿宝石（与红宝石 66% 完全相同）
      firered-leafgreen 火红/叶绿 —— 绘图质量最高，线条更干净、颜色更饱和
      black-white       第五世代黑白版 —— 与 Mega 图同源，风格统一；
                        但它是 96x96，需缩到 64x64，且只有正面

    返回 (记录列表, 冗余统计)
    记录 = dict(pid, kind, ident, src_path)
      kind: front / back / shiny / mega
    """
    bw = src / "generation-v" / "black-white"
    use_bw = (gen3_dir == "black-white")

    if use_bw:
        # 黑白版正面直接来自 generation-v/black-white/{pid}.png，
        # 与 Mega 同目录同尺寸（96x96），缩放后风格完全统一。
        # 该目录没有独立的三代子目录，所以 rs 也指向它。
        rs = bw
        if not rs.exists():
            raise SystemExit(f"源目录缺少 {rs}")
    else:
        rs = src / "generation-iii" / gen3_dir
        if not rs.exists():
            raise SystemExit(f"源目录缺少 {rs}")

    records = []
    stats = {
        "front": 0, "back": 0, "shiny": 0, "mega": 0,
        "redundant": 0, "missing": [],
    }

    # --- 正面 1..386 ---
    for pid in range(1, GEN3_MAX + 1):
        p = rs / f"{pid}.png"
        if p.exists():
            records.append(dict(pid=pid, kind="front", ident=normal_map.get(pid, ""),
                                src=p))
            stats["front"] += 1
        else:
            stats["missing"].append(f"front/{pid}")

    # --- 背面（可选。黑白版没有三代背面图；只要正面时整段跳过） ---
    if not use_bw and keep_back:
        bk = rs / "back"
        for pid in range(1, GEN3_MAX + 1):
            p = bk / f"{pid}.png"
            if p.exists():
                records.append(dict(pid=pid, kind="back", ident=normal_map.get(pid, ""),
                                    src=p))
                stats["back"] += 1
            else:
                stats["missing"].append(f"back/{pid}")

    # --- 三代闪亮（可选） ---
    if keep_shiny and not use_bw:
        sh = rs / "shiny"
        for pid in range(1, GEN3_MAX + 1):
            p = sh / f"{pid}.png"
            if p.exists():
                records.append(dict(pid=pid, kind="shiny",
                                    ident=normal_map.get(pid, ""), src=p))
                stats["shiny"] += 1
            else:
                stats["missing"].append(f"shiny/{pid}")

    # --- Mega：只保留白名单里的 41 个，其余全部算冗余 ---
    mega_pids = set(mega_map.keys())
    for p in sorted(bw.glob("*.png")):
        try:
            pid = int(p.stem)
        except ValueError:
            continue
        if pid in mega_pids:
            records.append(dict(pid=pid, kind="mega", ident=mega_map[pid][1], src=p))
            stats["mega"] += 1
        else:
            stats["redundant"] += 1

    # --- 三代目录里的冗余（超出 1..386 的文件） ---
    # 注意 use_bw 时 rs == bw，此时 front 循环已把 10000+ 段全过了一遍，
    # 不能再重复统计，否则冗余数会翻倍。
    if not use_bw:
        for d in (rs, rs / "back", rs / "shiny"):
            if not d.exists():
                continue
            for p in d.glob("*.png"):
                try:
                    pid = int(p.stem)
                except ValueError:
                    stats["redundant"] += 1
                    continue
                if not (1 <= pid <= GEN3_MAX):
                    stats["redundant"] += 1

    return records, stats


def export_raw(records):
    """把用到的原图复制到 data/sprites_raw/（其余不复制，即完成清理）。"""
    if RAW.exists():
        shutil.rmtree(RAW)
    for r in records:
        d = RAW / f"{r['kind']}"
        d.mkdir(parents=True, exist_ok=True)
        shutil.copy2(r["src"], d / f"{r['pid']:05d}.png")


# ---------------------------------------------------------------- 生成 C

def emit_array(name, data, comment):
    lines = [f"/* {comment} */", f"static const uint8_t {name}[] = {{"]
    for i in range(0, len(data), 16):
        chunk = data[i : i + 16]
        lines.append("    " + " ".join(f"0x{b:02X}," for b in chunk))
    lines.append("};")
    return "\n".join(lines)


KIND_LABEL = {"front": "正面", "back": "背面", "shiny": "闪亮", "mega": "Mega"}


def build_c(records, mega_map, gen3_dir="firered-leafgreen"):
    GEN.mkdir(parents=True, exist_ok=True)
    # 中间产物目录每次重建。只 mkdir 不清理的话，上一次用不同参数
    # 生成的图会留在里面——实测切回「只要正面」后目录里仍躺着386 张
    # back_*.png，看目录会误以为背面也在用。
    if Q4.exists():
        shutil.rmtree(Q4)
    Q4.mkdir(parents=True, exist_ok=True)

    rows = []      # (pid, kind, varname, w, h, bytes, pal)
    total_px = 0
    by_kind = {}

    for n, r in enumerate(records, 1):
        size = MEGA_SIZE if r["kind"] == "mega" else ICON_SIZE
        with Image.open(r["src"]) as im:
            # 三代图原生就是 64x64，Mega 是 96x96 需缩放
            img = fit_size(im, size) if im.size != (size, size) else im.convert("RGBA")
            q, alpha = quantize16(img)
            data = pack_4bpp(q, alpha)
            save_preview(q, alpha, Q4 / f"{r['kind']}_{r['pid']:05d}.png")

        pal = palette565(q)
        var = f"spr_{r['kind']}_{r['pid']:05d}"
        rows.append((r["pid"], r["kind"], var, size, size, data, pal, r["ident"]))
        total_px += len(data)
        by_kind[r["kind"]] = by_kind.get(r["kind"], 0) + len(data)

        if n % 100 == 0 or n == len(records):
            print(f"  {n}/{len(records)}  累计 {total_px/1024:.0f} KB", flush=True)

    # ---------- 头文件 ----------
    count = len(rows)
    # 来源描述跟着实际用的参数走，不要写死——之前模板里写死
    # ruby-sapphire，实际用的却是 firered-leafgreen，对不上。
    if gen3_dir == "black-white":
        src_desc = ("generation-v/black-white（96x96 原生，需缩放）")
        mega_src_note = f"96x96 缩至 {ICON_SIZE}x{ICON_SIZE}"
    else:
        src_desc = (f"generation-iii/{gen3_dir}"
                    f"（原生即 {ICON_SIZE}x{ICON_SIZE}，无需缩放）")
        mega_src_note = f"96x96 缩至 {ICON_SIZE}x{ICON_SIZE}"
    hdr = f"""/* 由 tools/build_sprites_c.py 自动生成，请勿手工编辑 */
/*
 * 精灵图 4bpp 索引色数据，全部 {ICON_SIZE}x{ICON_SIZE}。
 *
 * 来源  PokeAPI/sprites
 *   正面  {src_desc}
 *   Mega  generation-v/black-white（{mega_src_note}）
 *
 * 格式  每字节 2 像素（4 bit/像素），高位在前，行优先。
 *   像素 i 的索引 = (data[i/2] >> ((i%2) ? 0 : 4)) & 0xF
 *   索引 0 = 透明，不绘制。
 *
 * 构成  {by_kind.get('front', 0)//1024} KB 正面
 *   {by_kind.get('back', 0)//1024} KB 背面
 *   {by_kind.get('shiny', 0)//1024} KB 闪亮
 *   {by_kind.get('mega', 0)//1024} KB Mega
 *   合计 {total_px//1024} KB
 */

#include "poke_sprites.h"

"""
    (GEN / "poke_sprites.h").write_text(f"""/* 由 tools/build_sprites_c.py 自动生成 */
#ifndef POKE_SPRITES_H
#define POKE_SPRITES_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/* 调色板槽位：索引 0 = 透明 */
#define PALETTE_SIZE {PALETTE_COLORS}
#define PAL_TRANSPARENT 0

#define SPRITE_SIZE {ICON_SIZE}

typedef enum {{
    SPR_FRONT = 0,
    SPR_BACK  = 1,
    SPR_SHINY = 2,
    SPR_MEGA  = 3,
    SPR_MEGA_BACK = 4,
}} SpriteKind;

typedef struct {{
    uint16_t id;              /* pokemon id */
    uint8_t kind;             /* SpriteKind */
    const uint8_t *data;      /* 4bpp 像素，{ICON_SIZE}*{ICON_SIZE}/2 字节 */
    const uint16_t *palette;  /* RGB565，长度 PALETTE_SIZE */
    uint8_t w, h;
}} SpriteEntry;

#define SPRITE_COUNT {count}
extern const SpriteEntry kSpriteTable[SPRITE_COUNT];

/* 按 pokemon id + 图种查找，找不到返回 NULL */
const SpriteEntry *sprite_get(uint16_t pid, uint8_t kind);

#endif /* POKE_SPRITES_H */
""", encoding="utf-8")

    # ---------- 像素数据 ----------
    body = []
    body.append("/* ================= 像素数据 ================= */\n")
    for pid, kind, var, w, h, data, pal, ident in rows:
        label = KIND_LABEL[kind]
        cmt = f"pid={pid} {label} {w}x{h} {ident}"
        body.append(emit_array(var, data, cmt))

    # ---------- 调色板 ----------
    body.append("\n/* ================= 调色板 RGB565 ================= */\n")
    for pid, kind, var, w, h, data, pal, ident in rows:
        vals = ", ".join(f"0x{c:04X}" for c in pal)
        body.append(f"/* pid={pid} {KIND_LABEL[kind]} */")
        body.append(f"static const uint16_t {var}_pal[{PALETTE_COLORS}] = {{ {vals} }};\n")

    # ---------- 索引表 ----------
    body.append("/* ================= 索引表 ================= */\n")
    body.append(f"const SpriteEntry kSpriteTable[{count}] = {{")
    for pid, kind, var, w, h, data, pal, ident in rows:
        kc = {"front": "SPR_FRONT", "back": "SPR_BACK",
              "shiny": "SPR_SHINY", "mega": "SPR_MEGA"}[kind]
        body.append(f"    {{ {pid}, {kc}, {var}, {var}_pal, {w}, {h} }},")
    body.append("};\n")
    body.append("const SpriteEntry *sprite_get(uint16_t pid, uint8_t kind)")
    body.append("{")
    body.append("    for (unsigned i = 0; i < SPRITE_COUNT; i++) {")
    body.append("        if (kSpriteTable[i].id == pid && kSpriteTable[i].kind == kind) {")
    body.append("            return &kSpriteTable[i];")
    body.append("        }")
    body.append("    }")
    body.append("    return NULL;")
    body.append("}")

    (GEN / "poke_sprites.c").write_text(hdr + "\n".join(body) + "\n", encoding="utf-8")

    return count, total_px, by_kind


# ---------------------------------------------------------------- main

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--src", required=True,
                    help="PokeAPI/sprites 的 .../sprites/pokemon/versions 目录")
    ap.add_argument("--gen3", default="firered-leafgreen",
                    choices=["firered-leafgreen", "ruby-sapphire", "emerald",
                             "black-white"],
                    help="三代图源版本。默认火红/叶绿（绘图质量最高）；"
                         "black-white 用第五世代图，与 Mega 同源、仅正面")
    ap.add_argument("--shiny", action="store_true",
                    help="同时导入闪亮图（默认不导入，可省 386 KB）")
    ap.add_argument("--back", action="store_true",
                    help="同时导入背面图（默认不导入，可省 772 KB）。"
                         "只要正面时不要加这个开关")
    ap.add_argument("--keep-raw", action="store_true",
                    help="保留精简后的原图到 data/sprites_raw")
    args = ap.parse_args()

    src = Path(args.src)
    if not src.exists():
        raise SystemExit(f"源目录不存在: {src}")
    if not (src / "generation-v" / "black-white").exists():
        raise SystemExit("源目录缺少 generation-v/black-white")
    if args.gen3 != "black-white":
        need = f"generation-iii/{args.gen3}"
        if not (src / need).exists():
            raise SystemExit(f"源目录缺少 {need}")

    normal_map, mega_map = load_species_meta()
    print(f"图鉴号清单: 普通 {len(normal_map)} 只 / Mega {len(mega_map)} 个")
    if args.gen3 == "black-white":
        print("三代图源: generation-v/black-white（与 Mega 同源，风格统一，"
              "96x96 缩到 64x64，仅正面）")
    else:
        print(f"三代图源: generation-iii/{args.gen3}")

    print("\n扫描源目录 ...")
    records, stats = collect(src, normal_map, mega_map,
                             gen3_dir=args.gen3,
                             keep_shiny=args.shiny,
                             keep_back=args.back)
    print(f"  正面 {stats['front']}  背面 {stats['back']}  "
          f"闪亮 {stats['shiny']}  Mega {stats['mega']}")
    print(f"  冗余已排除 {stats['redundant']} 张")
    if stats["missing"]:
        print(f"  [warn] 缺失 {len(stats['missing'])}: {stats['missing'][:10]}")

    if args.keep_raw:
        print("\n导出精简后原图 ...")
        export_raw(records)
        print(f"  -> {RAW}")

    print("\n量化为 4bpp 并生成 C 数组 ...")
    count, total, by_kind = build_c(records, mega_map, gen3_dir=args.gen3)

    pal_bytes = count * PALETTE_COLORS * 2
    print(f"\n完成:")
    print(f"  条目 {count} 个 | 像素数据 {total/1024:.0f} KB "
          f"| 调色板 {pal_bytes/1024:.0f} KB")
    print(f"  Flash 占用约 {(total + pal_bytes)/1024:.0f} KB")
    sz = (GEN / "poke_sprites.c").stat().st_size
    print(f"  gen/poke_sprites.c  文本 {sz:,} B（含注释与调色板）")
    for k in ("front", "back", "shiny", "mega"):
        if k in by_kind:
            print(f"    {KIND_LABEL[k]:4s} {by_kind[k]/1024:>5.0f} KB")


if __name__ == "__main__":
    main()