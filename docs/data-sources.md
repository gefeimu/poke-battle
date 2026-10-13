# 数据来源与重建方法

仓库里**入库**的是生成产物（`gen/*.c`），**不入库**的是三份大的第三方数据。
这样克隆下来直接 `make test` 就能跑；只有要重新生成数据表时才需要把它们取回来。

| 目录 | 体积 | 为什么要用 | 入库 |
|---|---|---|---|
| `data/csv/` | ~12 MB | Veekun 的三代数据表，`tools/gen_gen3_data.py` 的输入 | ❌ |
| `data/fonts/` | 2.6 MB | 得意黑，`tools/gen_lvgl_font.py` 的输入 | ❌ |
| 精灵图源仓库 | 大 | PokeAPI/sprites，`tools/build_sprites_c.py` 的输入 | ❌ |
| `gen/*.c` | ~7 MB | 生成产物，固件真正编译的东西 | ✅ |
| `data/font_chars.txt` | 3.6 KB | 字库字符清单（字体生成的输入） | ✅ |

---

## 1. Veekun CSV → `data/csv/`

```bash
git clone --depth 1 https://github.com/veekun/pokedex /tmp/pokedex
mkdir -p data/csv
cp /tmp/pokedex/pokedex/data/csv/*.csv data/csv/      # 35 个文件
```

**注意一个坑**：`pokemon_moves.csv` 有约 638K 行。网络不稳时下载会被静默截断，
`gen_gen3_data.py` 不会报错，只是学习表里少一大批招式。生成前先验一下：

```bash
python - <<'PY'
import csv
n = sum(1 for _ in open('data/csv/pokemon_moves.csv', encoding='utf-8'))
print(n, "行（完整应 > 600000）")
PY
```

Veekun 的数据是 **CC-BY-SA 3.0**（见上游仓库 `README.md`）。

## 2. 得意黑 → `data/fonts/`

```bash
curl -L -o /tmp/smiley.zip \
  https://github.com/atelier-anchor/smiley-sans/releases/download/v2.0.1/smiley-sans-v2.0.1.zip
unzip -j /tmp/smiley.zip 'SmileySans-Oblique.ttf' -d data/fonts/
```

| 项 | 值 |
|---|---|
| 版本 | v2.0.1 |
| 文件 | `SmileySans-Oblique.ttf` |
| sha256 | `b447d7e781f08bc95c4c9f23ba71ed2b8ebb639aa7184485c71c4ca5afcd25c4` |
| 许可 | SIL Open Font License 1.1（全文见 `docs/third-party/SmileySans-OFL.txt`） |

**为什么选它而不是思源黑体**：`lv_font_conv` 内置的 opentype.js 读不了
**CFF 轮廓的大 OTF**（思源黑体 SC-Normal 15.6 MB 时报
`Offset is outside the bounds of the DataView`）。得意黑是 TrueType 轮廓，能读。
详见 README「中文字库」一节。

> 生成的 `gen/font_app_16.c` 是从这个字体裁出的**字形位图数据**，
> 属于 OFL 允许的嵌入使用；OFL 全文随仓库提供。

## 3. 精灵图 → `--src <路径>`

正面图取自 [PokeAPI/sprites](https://github.com/PokeAPI/sprites) 的
`sprites/pokemon/versions/` 目录。整个仓库很大（含各世代各版本），
但**只需要两个子目录**，用 sparse checkout：

```bash
git clone --depth 1 --filter=blob:none --sparse \
  https://github.com/PokeAPI/sprites /tmp/sprites
cd /tmp/sprites
git sparse-checkout set \
  sprites/pokemon/versions/generation-iii/firered-leafgreen \
  sprites/pokemon/versions/generation-v/black-white
```

然后：

```bash
make sprites SPRITES_SRC=/tmp/sprites/sprites/pokemon/versions
```

| 用途 | 取自 | 原生尺寸 |
|---|---|---|
| 正面立绘 386 张 | `generation-iii/firered-leafgreen` | 64×64（**无需缩放**） |
| Mega 41 张 | `generation-v/black-white` | 96×96 → 缩到 64×64 |

三代目录里**没有 Mega 图**（Mega 是第六代才有的形态），所以 Mega 只能取黑白版
再缩放。这是唯一的跨世代混用，且缩放后像素规格与正面统一（都是 64×64）。

PokeAPI/sprites 的图源是各代游戏素材，版权归任天堂 / Creatures / GAME FREAK。

---

## 重新生成全部数据的正确顺序

依赖关系是有向的，顺序错了会用到旧数据：

```bash
# 1. 数据表（依赖 data/csv）——含中文名，会顺带更新 tools/zh_names.py 的输出
python tools/gen_gen3_data.py

# 2. 属性克制表（纯硬编码，不依赖 CSV；改克制关系时才需要单独跑）
python tools/gen_gen3_data.py        # 已包含，或 make types

# 3. 字库字符清单必须晚于 1：它扫 gen/*.c 的中文名字面量
python tools/gen_font.py --font C:/Windows/Fonts/Deng.ttf   # 自制 1bpp
make lvgl-font FONT=data/fonts/SmileySans-Oblique.ttf       # LVGL 4bpp

# 4. 精灵图（依赖源仓库，与上面互不依赖）
make sprites SPRITES_SRC=<...>/sprites/pokemon/versions

# 5. 验收
make test              # 324 项断言 + 跨文件一致性检查
make sprites-preview   # 导图肉眼看精灵图
make font-preview      # 导图肉眼看字库
make readme-images     # 重出 README 配图（会顺带校验字库覆盖）
```

**第 3 步的顺序容易踩坑**：字符清单是从 `gen/*.c` 里扫中文字面量得到的，
所以必须先跑第 1 步。反过来先做字库，中文名一改，字库就缺字了
（`test_font.c` 会报出来）。
