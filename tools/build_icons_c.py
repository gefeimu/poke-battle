"""
从本地 sparse checkout 目录生成 32x32 宝可梦队伍小图标 4bpp C 数组。

输入（已由用户 sparse checkout 到本地）
    <src>/generation-iii/icons/{id}.png   原生 32x32 RGBA，id 1..386
    （目录里还有 201-a..z / 10001..10003 等形态文件，一律跳过）

处理
    1. 只取图鉴号 1..386 共 386 张，保持 32x32 原始构图（不缩放、不裁剪）
    2. 量化到 16 色（4bpp），透明像素阈值化为索引 0
    3. 打包 4bpp：每字节 2 像素（4 bit/像素），高位在前，行优先
    4. 调色板导成 RGB565 数组

复用 tools/build_sprites_c.py 的 quantize16 / pack_4bpp / palette565 /
save_preview / emit_array，避免两套流水线走样。

输出
    data/icons_raw/        原图（可选，--keep-raw 时导出，可删）
    data/icons_4bpp/       量化后的 PNG-8 中间产物（便于检查画质）
    gen/poke_icons.c/.h    固件用的 C 数组

用法
    python tools/build_icons_c.py \
        --src D:/Download/Chromedownload/sprites/sprites/pokemon/versions
"""

import argparse
import shutil
import sys
from pathlib import Path

from PIL import Image

sys.path.insert(0, str(Path(__file__).resolve().parent))
import build_sprites_c as bs  # noqa: E402  （main() 有 __main__ 保护，import 无副作用）

ROOT = Path(__file__).resolve().parent.parent
RAW = ROOT / "data" / "icons_raw"
Q4 = ROOT / "data" / "icons_4bpp"

ICON_MAX = 386
ICON_SIZE = 32
PALETTE_COLORS = 16


# ---------------------------------------------------------------- 收集

def collect(src):
    """扫描 <src>/generation-iii/icons，收集 id 1..386。

    返回 (记录列表, 缺失的 pid, 冗余跳过数)
    记录 = dict(pid, src_path)
    """
    idir = src / "generation-iii" / "icons"
    if not idir.exists():
        raise SystemExit(f"源目录缺少 {idir}")

    records, missing = [], []
    for pid in range(1, ICON_MAX + 1):
        p = idir / f"{pid}.png"
        if p.exists():
            records.append(dict(pid=pid, src=p))
        else:
            missing.append(pid)

    # 冗余统计：目录里非 1..386 的文件（形态图 / 数字以外的名字）一律跳过
    redundant = 0
    for p in idir.glob("*.png"):
        try:
            pid = int(p.stem)
        except ValueError:
            redundant += 1
            continue
        if not (1 <= pid <= ICON_MAX):
            redundant += 1

    return records, missing, redundant


def export_raw(records):
    """把用到的 386 张原图复制到 data/icons_raw/（其余不复制，即完成清理）。"""
    if RAW.exists():
        shutil.rmtree(RAW)
    RAW.mkdir(parents=True, exist_ok=True)
    for r in records:
        shutil.copy2(r["src"], RAW / f"{r['pid']:05d}.png")


# ---------------------------------------------------------------- 生成 C

def build_c(records, gen):
    gen.mkdir(parents=True, exist_ok=True)
    # 中间产物目录每次重建，避免上一次残留的图混在里面。
    if Q4.exists():
        shutil.rmtree(Q4)
    Q4.mkdir(parents=True, exist_ok=True)

    rows = []      # (pid, varname, bytes, pal)
    total_px = 0

    for n, r in enumerate(records, 1):
        with Image.open(r["src"]) as im:
            img = im.convert("RGBA")
            # 图标必须保持 32x32 原始构图：尺寸不对直接报错退出，不 trim/缩放。
            if img.size != (ICON_SIZE, ICON_SIZE):
                raise SystemExit(
                    f"图标 {r['src']} 尺寸为 {img.size}，应为 "
                    f"{ICON_SIZE}x{ICON_SIZE}；图标保持原始构图，不做缩放")
            q, alpha = bs.quantize16(img)
            data = bs.pack_4bpp(q, alpha)
            bs.save_preview(q, alpha, Q4 / f"{r['pid']:05d}.png")

        pal = bs.palette565(q)
        var = f"ico_{r['pid']:05d}"
        rows.append((r["pid"], var, data, pal))
        total_px += len(data)

        if n % 100 == 0 or n == len(records):
            print(f"  {n}/{len(records)}  累计 {total_px/1024:.0f} KB", flush=True)

    count = len(rows)

    # ---------- 头文件 ----------
    hdr = f"""/* 由 tools/build_icons_c.py 自动生成，请勿手工编辑 */
/*
 * 32x32 宝可梦队伍小图标（来源 PokeAPI/sprites 的 generation-iii/icons ——
 * 原生 32x32 火红/叶绿队伍图标，无需缩放，保持原始构图）。
 *
 * 格式与精灵图（poke_sprites.h）相同：
 *   4bpp，每字节 2 像素，高位在前，行优先；
 *   像素 i 的索引 = (data[i/2] >> ((i%2) ? 0 : 4)) & 0xF；
 *   索引 0 = 透明，不绘制。
 *
 * 构成  {count} 张，每张 {ICON_SIZE*ICON_SIZE//2} B 像素 + 16 色 RGB565 调色板
 */
#ifndef POKE_ICONS_H
#define POKE_ICONS_H

#include <stdint.h>
#include "poke_sprites.h"     /* 复用 SpriteEntry */

#define ICON_SIZE  {ICON_SIZE}
#define ICON_COUNT {count}

extern const SpriteEntry kIconTable[ICON_COUNT];

/* 按图鉴号取 32x32 队伍小图标；找不到（0 或 >{ICON_MAX}）返回 NULL */
const SpriteEntry *icon_get(uint16_t pid);

#endif /* POKE_ICONS_H */
"""
    (gen / "poke_icons.h").write_text(hdr, encoding="utf-8")

    # ---------- 实现文件 ----------
    body = ['#include "poke_icons.h"', ""]

    body.append("/* ================= 像素数据 ================= */\n")
    for pid, var, data, pal in rows:
        body.append(bs.emit_array(var, data, f"pid={pid} {ICON_SIZE}x{ICON_SIZE}"))

    body.append("\n/* ================= 调色板 RGB565 ================= */\n")
    for pid, var, data, pal in rows:
        vals = ", ".join(f"0x{c:04X}" for c in pal)
        body.append(f"/* pid={pid} */")
        body.append(f"static const uint16_t {var}_pal[{PALETTE_COLORS}] = {{ {vals} }};\n")

    body.append("/* ================= 索引表 ================= */\n")
    body.append(f"const SpriteEntry kIconTable[ICON_COUNT] = {{")
    for pid, var, data, pal in rows:
        body.append(f"    {{ {pid}, 0, {var}, {var}_pal, {ICON_SIZE}, {ICON_SIZE} }},")
    body.append("};\n")

    body.append("const SpriteEntry *icon_get(uint16_t pid)")
    body.append("{")
    body.append("    for (unsigned i = 0; i < ICON_COUNT; i++) {")
    body.append("        if (kIconTable[i].id == pid) {")
    body.append("            return &kIconTable[i];")
    body.append("        }")
    body.append("    }")
    body.append("    return NULL;")
    body.append("}")

    (gen / "poke_icons.c").write_text("\n".join(body) + "\n", encoding="utf-8")

    return count, total_px


# ---------------------------------------------------------------- main

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--src", required=True,
                    help="PokeAPI/sprites 的 .../sprites/pokemon/versions 目录"
                         "（其下 generation-iii/icons 存放 32x32 队伍小图标）")
    ap.add_argument("--keep-raw", action="store_true",
                    help="保留原图到 data/icons_raw")
    ap.add_argument("--out-dir", default="gen",
                    help="C 输出目录（默认 gen）")
    args = ap.parse_args()

    src = Path(args.src)
    if not src.exists():
        raise SystemExit(f"源目录不存在: {src}")

    out_dir = Path(args.out_dir)
    if not out_dir.is_absolute():
        out_dir = ROOT / out_dir

    print("扫描图标目录 ...")
    records, missing, redundant = collect(src)
    print(f"  命中 {len(records)}  冗余跳过 {redundant}")
    if missing:
        print(f"  [warn] 缺失 {len(missing)} 张: {missing[:10]}")
    if len(records) != ICON_MAX:
        print(f"  [warn] 期望 {ICON_MAX} 张，实际得到 {len(records)} 张")

    if args.keep_raw:
        print("\n导出原图 ...")
        export_raw(records)
        print(f"  -> {RAW}")

    print("\n量化为 4bpp 并生成 C 数组 ...")
    count, total = build_c(records, out_dir)

    pal_bytes = count * PALETTE_COLORS * 2
    print("\n完成:")
    print(f"  条目 {count} 个 | 像素数据 {total/1024:.0f} KB "
          f"| 调色板 {pal_bytes/1024:.0f} KB")
    print(f"  Flash 占用约 {(total + pal_bytes)/1024:.0f} KB")
    sz = (out_dir / "poke_icons.c").stat().st_size
    print(f"  gen/poke_icons.c  文本 {sz:,} B（含注释与调色板）")


if __name__ == "__main__":
    main()
