"""
ppm2png.py - PPM(P6) 转 PNG，可选放大

固件渲染层导出的是 PPM：格式简单到可以手写（P6 + RGB888 裸数据），
PIL 原生支持读取，一条命令就能转 PNG 并放大 —— 比在 C 里手写 PNG
编码器靠谱（之前写字体预览时，手写 PNG 的 zlib 存储块超过 65535 字节
上限，输出过一个损坏文件）。

用法：
    python tools/ppm2png.py 输入.ppm 输出.png [--scale 3]
"""

import argparse
import sys
from pathlib import Path

from PIL import Image


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("src")
    ap.add_argument("dst")
    ap.add_argument("--scale", type=int, default=1, help="整数倍放大（最近邻）")
    args = ap.parse_args()

    im = Image.open(args.src)
    if im.mode != "RGB":
        im = im.convert("RGB")
    if args.scale > 1:
        im = im.resize((im.width * args.scale, im.height * args.scale),
                       Image.NEAREST)
    Path(args.dst).parent.mkdir(parents=True, exist_ok=True)
    im.save(args.dst)
    print(f"{args.src} -> {args.dst}（{im.width}x{im.height}）")
    return 0


if __name__ == "__main__":
    sys.exit(main())
