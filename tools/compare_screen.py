"""
compare_screen.py - 比对「固件渲染层」与「PC 预览」画出来的同一屏

两套渲染是**独立实现**：
  * 固件侧 src/poke_render.c + src/poke_ui.c，输出 RGB565 帧缓冲
    （test/dump_screen.c 导成 PPM）
  * PC 侧 tools/make_readme_images.py，PIL 画 RGB888

它们遵循同一份版式数值与同一套字体数据，所以**应该高度一致但不是逐字节
相同**。差异有三种来源，都是结构性的、不是 bug：

1. **色彩深度**：固件是 RGB565，每通道 5/6/5 位；PC 是 RGB888。
   所以每个像素天然有最多 ±4（R/B）/ ±2（G）的量化误差。比对前把 PC 图
   也压到 RGB565 再还原，消掉这一项。
2. **边缘 1px**：PIL 的圆角用椭圆弧切，我们用自己的圆判定；抗锯齿的
   混合也在不同色深下做。这类差异集中在轮廓边缘，允许 1px 位置容差。
3. **渐变插值空间**：我们在 RGB565 空间插值，PIL 在 RGB888 空间插值，
   同一点最多差 1 个 565 台阶（8 个 888 台阶）。

验收标准：**带 1px 位置容差后，差异 > 32 的像素应低于 2%**。
差异 > 32 意味着「一边有内容、另一边没有」，那是真问题（漏画、错位、
颜色错档），必须查。

用法：
    python tools/compare_screen.py \\
        --c data/screen_preview/battle.ppm \\
        --py docs/img/hero.png --scale 3
"""

import argparse
import sys
from pathlib import Path

import numpy as np
from PIL import Image

# 差异超过这个值就算「实质性不同」：1 个 565 台阶在 R/B 上是 8 个 888 台阶，
# 取 32 是 4 个台阶，明显超出量化与插值能解释的范围。
BIG_DIFF = 32
BIG_DIFF_LIMIT = 0.02      # 2%


def to_565_roundtrip(a):
    """把 RGB888 压到 RGB565 再还原，模拟固件的色彩深度。"""
    q = np.stack([
        np.round(a[:, :, 0] / 255 * 31),
        np.round(a[:, :, 1] / 255 * 63),
        np.round(a[:, :, 2] / 255 * 31),
    ], axis=2).astype(np.int16)
    return np.stack([
        (q[:, :, 0] << 3) | (q[:, :, 0] >> 2),
        (q[:, :, 1] << 2) | (q[:, :, 1] >> 4),
        (q[:, :, 2] << 3) | (q[:, :, 2] >> 2),
    ], axis=2).astype(np.int16)


def load(path, scale=1):
    im = Image.open(path).convert("RGB")
    a = np.asarray(im, dtype=np.int16)
    if scale > 1:
        # 精确整数倍降采样。不要用 resize(NEAREST)：PIL 缩小时的采样位置
        # 带半像素偏移，会让每个边缘都差 1px，白算出几十个百分点的差异。
        a = a[::scale, ::scale]
    return a


def neighborhood_min_diff(c, ref, radius=1):
    """每个像素与对方 (2r+1)^2 邻域里最接近的像素比，容忍 1px 位置差。"""
    h, w, _ = c.shape
    pad = np.pad(ref, ((radius, radius), (radius, radius), (0, 0)), mode="edge")
    best = np.full((h, w), 255, dtype=np.int16)
    for dy in range(-radius, radius + 1):
        for dx in range(-radius, radius + 1):
            sh = pad[radius + dy:radius + dy + h, radius + dx:radius + dx + w]
            best = np.minimum(best, np.abs(c - sh).max(axis=2))
    return best


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--c", required=True, help="固件渲染层导出的 PPM")
    ap.add_argument("--py", required=True, help="PC 预览的 PNG")
    ap.add_argument("--scale", type=int, default=3, help="PC 预览的放大倍数")
    ap.add_argument("--heatmap", help="差异热力图输出路径（可选）")
    args = ap.parse_args()

    c = load(args.c)
    b = load(args.py, args.scale)
    if c.shape != b.shape:
        raise SystemExit(f"尺寸不一致：C={c.shape} Py={b.shape}"
                         f"（--scale 是不是填错了？）")

    h, w, _ = c.shape
    total = h * w
    print(f"尺寸 {w}x{h}，共 {total:,} 像素")

    bq = to_565_roundtrip(b)

    d_raw = np.abs(c - b).max(axis=2)
    d_q = np.abs(c - bq).max(axis=2)
    print(f"\n[1] 直接比（不处理色彩深度差异）")
    print(f"    任意差异 {(d_raw > 0).sum() / total * 100:5.1f}%   "
          f">32 {(d_raw > BIG_DIFF).sum() / total * 100:5.2f}%")
    print(f"[2] 把 PC 图压到 RGB565 后比（消掉量化差异）")
    print(f"    任意差异 {(d_q > 0).sum() / total * 100:5.1f}%   "
          f">32 {(d_q > BIG_DIFF).sum() / total * 100:5.2f}%")

    best = neighborhood_min_diff(c, bq)
    print(f"[3] 再允许 1px 位置容差（边缘对齐差异）")
    for thr in (8, 16, BIG_DIFF, 64):
        print(f"    >{thr:3d}: {(best > thr).sum() / total * 100:5.2f}%")

    ratio = (best > BIG_DIFF).sum() / total
    print(f"\n验收：差异 >{BIG_DIFF} 的像素 {ratio * 100:.2f}%"
          f"（阈值 {BIG_DIFF_LIMIT * 100:.0f}%）")

    ys, xs = np.where(best > BIG_DIFF)
    if len(ys):
        # 按行带分组，便于定位是哪一块出的问题
        bands = [("场景 / 血条框", 0, 160), ("对话框", 160, 220),
                 ("招式菜单", 220, 288), ("状态行", 288, h)]
        print(f"\n超出阈值的像素分布（共 {len(ys)} 个）：")
        for name, y0, y1 in bands:
            n = int(((ys >= y0) & (ys < y1)).sum())
            if n:
                print(f"    {name:14s} {n:5d}")
        print("  前 6 个：")
        for i in range(min(6, len(ys))):
            y, x = int(ys[i]), int(xs[i])
            print(f"    ({x:3d},{y:3d})  C={tuple(int(v) for v in c[y, x])}"
                  f"  Py={tuple(int(v) for v in bq[y, x])}")

    if args.heatmap:
        Image.fromarray(np.clip(best, 0, 255).astype(np.uint8)).resize(
            (w * 2, h * 2), Image.NEAREST).save(args.heatmap)
        print(f"\n热力图 {args.heatmap}")

    if ratio > BIG_DIFF_LIMIT:
        print("\n结果：**不通过**")
        return 1
    print("\n结果：通过（结构与配色一致，差异都在量化/边缘容差内）")
    return 0


if __name__ == "__main__":
    sys.exit(main())
