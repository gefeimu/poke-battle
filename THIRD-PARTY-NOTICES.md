# 第三方声明（Third-Party Notices）

本仓库的 **MIT 许可（见 [LICENSE](LICENSE)）只覆盖本项目自己写的代码**
（`src/`、`test/`、`tools/`、`Makefile` 等）。

仓库里还包含**第三方内容的衍生物**，它们不受 MIT 覆盖，另有各自的条款。
单独写成本文件而不是塞进 `LICENSE` 末尾，是为了让 GitHub 能把仓库正确识别为
MIT（在 `LICENSE` 里追加任何额外文字都会让它判成 "Other"）。

## 仓库内受第三方条款约束的文件

| 文件 | 内容 | 条款 |
|---|---|---|
| `gen/poke_sprites.c` | 427 张精灵图的索引色像素与调色板 | 游戏素材，版权归任天堂 / Creatures / GAME FREAK；来源见 [docs/data-sources.md](docs/data-sources.md) |
| `gen/font_app_16.c` | 得意黑（Smiley Sans）裁出的字形位图 | SIL OFL 1.1，全文见 [docs/third-party/SmileySans-OFL.txt](docs/third-party/SmileySans-OFL.txt) |
| `gen/font_cn.c` | 等线（Windows 系统字体）裁出的点阵 | 随 Windows 分发，**仅供本机自用**；若要公开发布请换成 OFL 字体重新生成 |

**下载到本地才会用到的数据**（不在仓库里，见 [docs/data-sources.md](docs/data-sources.md)）：

| 资源 | 用途 | 条款 |
|---|---|---|
| Veekun 数据集 | 三代种族 / 招式 / 特性 / 道具 / 性格 | CC-BY-SA 3.0 |
| PokeAPI/sprites | 精灵图源文件 | 游戏素材，版权归任天堂 / Creatures / GAME FREAK |
| 得意黑 Smiley Sans | 字库的源字体 | SIL OFL 1.1 |

## 商标

宝可梦（Pokémon）是任天堂 / Creatures / GAME FREAK 的注册商标，相关名称、形象与
招式设计的权利归其各自所有者。本项目为**自用学习项目，不作商业用途**，
与上述公司无任何关联，也未获其授权或背书。
