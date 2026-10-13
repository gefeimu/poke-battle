"""
只下载项目真正需要的精灵图。

不用打包下载的原因：
  - PokeAPI/sprites 仓库打包后超过 1.5GB，其中 99.99% 是不需要的高分辨率素材
  - jsDelivr 的目录打包 API 对该仓库返回 403（超过 50MB 限制）
  - 但 jsDelivr 的单文件 CDN 完全可用，所以走逐个下载

实际需要的：
  generation-iii/ruby-sapphire/{id}.png        1..386   64x64 正面
  generation-iii/ruby-sapphire/{id}_back.png   1..386   64x64 背面
  generation-v/black-white/{mega_pid}.png      52 个   96x96 Mega 立绘

合计约 1 MB，几分钟可完成。

用法
----
    python tools/fetch_sprites_local.py                  # 全部
    python tools/fetch_sprites_local.py --only mega      # 只下 Mega
    python tools/fetch_sprites_local.py --only gen3      # 只下三代
    python tools/fetch_sprites_local.py --threads 2      # 降并发(网络差时)
"""

import argparse
import re
import sys
import time
import urllib.request
from concurrent.futures import ThreadPoolExecutor, as_completed
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
RAW = ROOT / "data" / "sprites_raw"
GEN = ROOT / "gen"

GEN3_BASE = (
    "https://cdn.jsdelivr.net/gh/PokeAPI/sprites@master/"
    "sprites/pokemon/versions/generation-iii/ruby-sapphire"
)
GEN5_BASE = (
    "https://cdn.jsdelivr.net/gh/PokeAPI/sprites@master/"
    "sprites/pokemon/versions/generation-v/black-white"
)
# 备用源：GitHub raw（本机代理对 raw.githubusercontent 不通，但可作为换网络时的选择）
GEN3_BASE_GH = (
    "https://raw.githubusercontent.com/PokeAPI/sprites/master/"
    "sprites/pokemon/versions/generation-iii/ruby-sapphire"
)
GEN5_BASE_GH = (
    "https://raw.githubusercontent.com/PokeAPI/sprites/master/"
    "sprites/pokemon/versions/generation-v/black-white"
)

UA = {"User-Agent": "Mozilla/5.0 (Windows NT 10.0; Win64; x64)"}
GEN3_MAX = 386


def load_mega_pids():
    """从生成好的种族表里读 Mega 的 pokemon id 列表。"""
    src = GEN / "poke_species.c"
    if not src.exists():
        raise SystemExit(
            "找不到 gen/poke_species.c，请先运行 tools/gen_gen3_data.py"
        )
    text = src.read_text(encoding="utf-8")
    out = []
    for pid, _sid, ident, _gen, is_mega in re.findall(
        r'\{ (\d+), (\d+), "([\w-]+)", (\d+), (\d),', text
    ):
        if is_mega == "1":
            out.append((int(pid), ident))
    return sorted(out)


def fetch(url, dest, retries=6, quiet=True):
    """下载单个文件，已存在则跳过。返回 True/False。"""
    dest.parent.mkdir(parents=True, exist_ok=True)
    if dest.exists() and dest.stat().st_size > 0:
        return True
    delay = 1.2
    for attempt in range(retries):
        try:
            req = urllib.request.Request(url, headers=UA)
            with urllib.request.urlopen(req, timeout=40) as r:
                data = r.read()
            if not data:
                raise OSError("empty")
            dest.write_bytes(data)
            return True
        except Exception as exc:
            if not quiet:
                print(f"    retry{attempt + 1} {url}: {exc}")
            time.sleep(delay)
            delay = min(delay * 1.5, 8.0)
    return False


def build_jobs(only):
    """构造 (说明, 相对路径, url) 列表。"""
    jobs = []

    if only in ("all", "gen3"):
        for pid in range(1, GEN3_MAX + 1):
            jobs.append((
                f"gen3-front-{pid}",
                f"gen3_front/{pid:04d}.png",
                f"{GEN3_BASE}/{pid}.png",
                f"{GEN3_BASE_GH}/{pid}.png",
            ))
            jobs.append((
                f"gen3-back-{pid}",
                f"gen3_back/{pid:04d}_back.png",
                f"{GEN3_BASE}/{pid}_back.png",
                f"{GEN3_BASE_GH}/{pid}_back.png",
            ))

    if only in ("all", "mega"):
        for pid, ident in load_mega_pids():
            jobs.append((
                f"mega-{ident}",
                f"mega/{ident}_{pid:05d}.png",
                f"{GEN5_BASE}/{pid}.png",
                f"{GEN5_BASE_GH}/{pid}.png",
            ))

    return jobs


def run_pool(jobs, raw_dir, workers, start_fail):
    ok, fail = 0, []
    with ThreadPoolExecutor(max_workers=workers) as pool:
        futs = {}
        for name, rel, url, url_bak in jobs:
            futs[pool.submit(fetch, url, raw_dir / rel)] = (name, rel, url_bak)
        done = 0
        for fut in as_completed(futs):
            name, rel, url_bak = futs[fut]
            good = fut.result()
            if not good:
                # 换备用源再试一次
                good = fetch(url_bak, raw_dir / rel, retries=3)
            done += 1
            if good:
                ok += 1
                if start_fail and ok == 1:
                    print("  首个文件下载成功，链路正常")
            else:
                fail.append(name)
            if done % 40 == 0 or done == len(jobs):
                pct = done * 100 // max(len(jobs), 1)
                print(f"  {done}/{len(jobs)} ({pct}%)  成功 {ok}  失败 {len(fail)}", flush=True)
    return ok, fail


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--only", choices=["all", "gen3", "mega"], default="all")
    ap.add_argument("--threads", type=int, default=3,
                    help="并发数。默认 3；网络不稳时可降到 1~2")
    ap.add_argument("--out", default=None, help="输出目录，默认 data/sprites_raw")
    args = ap.parse_args()

    raw_dir = Path(args.out) if args.out else RAW

    jobs = build_jobs(args.only)
    total_bytes_needed = len(jobs)
    print(f"目标目录: {raw_dir}")
    print(f"待下载 {total_bytes_needed} 个文件，约 1 MB")
    print(f"并发 {args.threads} 线程")
    print()

    t0 = time.time()
    ok, fail = run_pool(jobs, raw_dir, args.threads, start_fail=True)
    elapsed = time.time() - t0

    print()
    print(f"完成: 成功 {ok} / {total_bytes_needed}，耗时 {elapsed:.0f}s")
    if fail:
        print(f"失败 {len(fail)} 个: {fail[:10]}")
        print("再跑一次本脚本即可补齐（已下载的会跳过）")
        sys.exit(1)

    # 统计
    files = list(raw_dir.rglob("*.png"))
    size = sum(f.stat().st_size for f in files)
    print(f"目录现有 {len(files)} 个 PNG，合计 {size/1024:.0f} KB")
    for sub in ("gen3_front", "gen3_back", "mega"):
        fs = list((raw_dir / sub).rglob("*.png")) if (raw_dir / sub).exists() else []
        if fs:
            print(f"  {sub:12s} {len(fs)} 张")


if __name__ == "__main__":
    main()