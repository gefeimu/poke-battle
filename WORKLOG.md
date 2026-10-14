# poke-battle 工作记忆（WORKLOG）

> 这份文件记录项目的**开发工作记忆**：当前状态、关键决策、踩过的坑、下一步。
> 面向的场景是「隔一段时间回来，快速接上」——不是使用者说明书。
>
> - 技术细节与使用说明 → `README.md`
> - 数据源来龙去脉 → `docs/data-sources.md`
> - 本文档 → 进度 / 决策 / 教训 / 待办
>
> **最后更新：2026-10-14**

---

## 0. 一页速览

| 项 | 内容 |
|---|---|
| 项目 | 三代图鉴 1..386（+41 Mega）的 6v6 宝可梦对战，跑在 **FoloToy AI Passport**（成品硬件，ESP32-C3） |
| 规则世代 | **第六代（X/Y ORAS）**，宝可梦范围三代图鉴，Mega 永久形态（按三代处理） |
| 屏幕 | 240×320 RGB565，官方固件是 **9.6 KB 条纹缓冲**（不是全屏帧缓冲） |
| 输入 | **三个键共用一个 ADC 引脚**（电压分档），自己写驱动，不用官方按键组件 |
| 当前状态 | 核心链路**全部打通**，PC 上能玩整局；剩队伍配置 / NVS 存档 / 接官方 BSP |
| 质量基线 | **983 项断言全过、17 组一致性检查 0 处不一致、零编译警告** |
| 最先要看的文件 | `README.md`（全景）→ `src/poke_session.c`（把各层串起来的地方） |
| 最先要跑的命令 | `make test`（一键全测） / `make play`（开窗口试玩） |

**一句话**：数据层、规则层、AI、渲染、输入、状态机、会话层都已经完成且可在 PC 上跑断言，
唯一没做的是「让玩家自己配队」「存档」「换成官方板级初始化」这三件事。

---

## 1. 工程位置与环境

### 1.1 工作目录（2026-10-14 迁移过）

```
D:\Project\C\poke-battle          ← 唯一工作目录，所有开发在这里
```

迁移前的旧址 `C:\Users\baigu\WorkBuddy\2026-10-12-14-17-04\poke-battle` 已不在项目列表内。
（该目录因本机安全删除机制拦截，物理上仍可能残留一份，**不要再去那里改代码**——
以 `D:\Project\C\poke-battle` 为唯一真源。）

同目录下另有 `D:\Project\C\poke-battle.old-clone-20261014`，是迁移前的一个旧 clone 备份
（停在 `a2ff28e`，无独有内容），确认新目录无误后可自行删除。

### 1.2 Git 配置（重要，本机全局配置是错的）

```bash
# 本机 git 全局身份是错的：yunli <916417497@qq.com>
# 本仓库已在 local 层设对，不要动：
git config --local --get user.name    # → 歌斐木
git config --local --get user.email   # → baiguiyeyou@outlook.com
```

- 远端：`https://github.com/gefeimu/poke-battle.git`（`origin`，分支 `main`）
- 当前 HEAD：`0a9873f`，本地领先 `origin/main` 7 个提交（未推送），本地共 **22 个提交**
- **新建/克隆仓库后第一件事**：`git config --local user.name/user.email` 改对，
  否则提交者身份是错的，事后改历史非常麻烦。

### 1.3 本机环境注意

- 访问 GitHub 依赖代理，**会间歇性 502**；`git push` / `ls-remote` 失败时多试几轮，别急着判定代码问题。
- ⚠️ **严禁在本机对仓库执行 `git stash push -u` 和 `git rebase`**：实测会段错误并损坏
  `.git`（丢 refs、丢 pack、工作区文件凭空消失）。需要验证中间态就用 `git worktree add`
  或依赖最终态验证；需要重写历史就用 `git commit-tree` 纯 plumbing，全程不碰工作区。
- 本机 MinGW 产物带 `.exe` 后缀——写脚本找二进制时要注意（`check_consistency.py` 第 [9] 组踩过）。

---

## 2. 质量基线（2026-10-14 在 D 盘新目录实测）

### 2.1 断言与一致性

| 测试程序 | 断言数 | 测试程序 | 断言数 |
|---|---|---|---|
| `data_selfcheck` | 110 | `test_font` | 25 |
| `test_calc` | 131 | `test_render` | 105 |
| `test_battle` | 100 | `test_input` | 51 |
| `test_ai` | 19 | `test_ui` | 125 |
| `test_sprites` | 24 | `test_session` | 375 |
| | | **合计** | **1065** |

- `make test` → 1065 项全过，零编译警告
- `make check-consistency` → **17 组跨文件检查，0 处不一致**
- `make play-check` → PC 试玩窗口无头自检 15/15

### 2.2 AI 实测强度

| | 样本 | 结果 |
|---|---|---|
| AI（冠军档）vs 随机手 | 200 局 | **136/200 = 68%** |
| 随机手基线（同批种子） | 200 局 | 103/200 = 52% |
| AI vs 随机手（大样本） | 1000 局 | 671/1000 = 67.1% |

断言写成「AI 胜率 − 基线胜率 ≥ 10 个百分点」，必须带对照组（6v6 队伍强度方差很大）。

> `README.md` 里的数字已于 2026-10-14 同步（修正了残留的「70% (139/200)」与「16 组」）。
> 注意 `check_consistency.py` 第 [9] 组只比对断言总数，**AI 胜率、一致性组数这类数字它覆盖不到**，
> 改动这些数字时必须手动同步文档。

### 2.3 多队伍对战基准（2026-10-14 新增）

`make bench-teams`，一次完整跑约 **6400 局 / 9 分钟**（`--quick` / `--games N` / `--csv PATH`）。
原始数据：`data/ai_bench/results.csv`（30 行）。

**三条最值得记住的结论：**

1. **三个 AI 档位在强度上分不出来。** ROOKIE 66.4% / SMART 67.0% / EXPERT 66.6%（各 N=500，sd≈2.1%）。
   这与 `test_ai` 的「档位差异」断言不矛盾——那条断言的是**行为**有差异（ROOKIE 有 15% 失手），
   不是强度有差异。即：现在是「玩法有区别、胜率没区别」。需求写的是 5 档难度。
2. **队伍种族值的影响远大于 AI 档位。** 肉盾/强攻队 vs 速攻队能到 74%，
   而 AI 档位之间差不到 5 个百分点。注意基准里的原型池是「按种族值排前 24」（全是准神/神兽），
   RANDOM 是 1..386 均匀抽，所以那组数据说明的是「种族值能碾压」，不是「某风格克制某风格」。
3. **AI vs AI 的 side0 偏置：随机源 bug 已修，但仍有残留，未定位。**
   - **已修（2026-10-14）**：测试构建里 `ai_arena.h` 自建了一份 `static s_rng`，而
     `ai_choose` 是靠 `battle_rng_state()` 保存/恢复随机流的——两者指向**不同的变量**，于是
     搜索的 save/restore 在测试里形同虚设，AI vs AI 时进入 `battle_step` 的随机流只由
     **后动作那一侧**的搜索长度决定（真机没这问题：1104/1113/1276 保存恢复的是同一个变量）。
     修法：`ai_arena.h` 的 `battle_rng()`/`calc_rand_85_100()`/`rng_reset()` 一律改走
     `battle_rng_state()`，不再自建。修后**套 2 对角格 ≈ 50%**（50.0 / 51.3 / 46.7%，N=150）。
     `test_ai` 的 68% 与 README 胜率不受影响（AI vs 随机只抽 1 次，后手 `rng_reset()` 本就
     覆盖掉先手的消耗）。
   - **残留（待办）**：套 3 里唯一**真镜像**的格子（SPEED 行，`i==j==0` 时 `i*101 == j*977 == 0`，
     两队种子确实相同）side0 仍只赢 **26.7%**（N=150，远偏离 50%）。`who_first()` 的同速判定
     是公平随机（[`poke_battle.c`](file:///d:/Project/C/poke-battle/src/poke_battle.c#L896)），
     故**不是出手顺序**。方向未定：怀疑要么是 AI 对 side0/side1 的处理不对称，要么是 arena
     每次决策前 `rng_reset()` 使进入 `battle_step` 的随机流每回合都从同一位置重开、把
     「同速随机」退化成确定性。**定位前，套 3 的对角格都不能当无偏对照**；且只有 SPEED 行
     是真镜像（`i*101` vs `j*977` 对 i>0 不相等，先前记的「真镜像」有误）。

---

## 3. 里程碑提交序列

```
c1117dc  首次提交：三代宝可梦对战引擎（数据层 + 规则层 + AI）
dd37a91  docs: 展开 README 正文，新增界面设计 / 路线图 / 许可章节
91e0116  ui: 战斗界面改为三代原版版式（尺寸与配色从反编译仓查证）
8cee5a0  lic: 补 MIT 许可（含第三方素材的范围说明）
8159e01  lic: LICENSE 保持纯净 MIT，第三方范围说明移到 NOTICE 文件
053818e  ui: 按预览反馈调整战斗界面（血条变细圆角、小字号、去 EXP、站台对齐）
f479d70  ui: 「HP」标签嵌进血条，血条框再压紧
265f445  ui: 血条再细一档、状态框留 2px 边距、巨金怪也加 HP 标、标签小一号
9ee9e2f  ui: 渲染层最小闭环（绘制原语 + 位图字体 + 战斗界面）
162f491  input: 三键驱动 + 界面状态机（喂电压/按键序列即可验收）
4c43d24  ui: 弹层第二项「逃跑」改为「认输」
54a4873  ui: 认输加一道二次确认界面
4facb4e  session+ui: 会话层接引擎，菜单七屏绘制，克制表改由官方 CSV 生成
a2ff28e  play: 加 PC 试玩窗口（make play）——硬件到手前先能上手打一局
89f7c9e  fix: 能力值公式/等级倍率/性格三处错值，并给数值模型加外部已知值回归
85cb79e  bench: 加多队伍 AI 对战基准（档位/风格矩阵 + 大样本胜率），对局脚手架与 test_ai 共用
59eaf9d  fix: 修正 ai_choose 兜底块的缩进与损坏注释字符
f37cc7a  fix: 统一 test/ai_arena.h 的随机源（改走 battle_rng_state），消除 AI vs AI 的随机流耦合   ← HEAD
```

---

## 4. 代码架构

### 4.1 目录职责

```
src/     手写源码（唯一真源）
gen/     生成物（数据表 / 精灵图 / 字库），由 tools/ 生成，不要手改
tools/   生成器 + 权威校验工具（Python）
test/    测试与导出工具（每个 .c 一个测试程序或预览导出）
data/    CSV 数据源、字体 TTF、预览图、基准结果
docs/    数据源说明、第三方素材
```

### 4.2 `src/` 各文件职责

| 文件 | 职责 |
|---|---|
| `poke_calc.c/h` | 能力值（含 HP 公式、性格、努力值）、属性克制、完整伤害链、Mega 切换 |
| `poke_battle.c/h` | 对战引擎：6v6 回合流程、状态、招式效果分派、异常/回复/天气 |
| `poke_ability.c/h` | 特性系统（251 个） |
| `poke_item.c/h` | 道具系统 |
| `poke_ai.c/h` | 三层 AI（L1 启发式 / L2 2-ply / L3 蒙特卡洛） |
| `poke_render.c/h` | 渲染层：绘制原语 + 精灵图 + 位图字体 |
| `poke_ui.c/h` | 界面绘制：战斗界面 + 菜单界面（八屏） |
| `poke_ui_state.c/h` | 界面状态机：界面栈 / 光标 / 滚动 / 连发 / 动作上报 |
| `poke_input.c/h` | 三键驱动：键位锁 + 去抖 + 短按/长按 |
| `poke_session.c/h` | 会话层：按键 → 状态机 → 引擎 → 一屏数据（把上面全串起来） |
| `poke_font.c/h` | 字库访问（按码点二分查找） |

**尚未存在**（路线图待做）：`src/poke_save.c/.h`（NVS 存档）、`main/app_main.c`（固件入口 / 板级初始化）。

### 4.3 常用命令

```bash
make test              # 全部测试 + 一致性检查（主力命令）
make play              # 开 Windows 窗口试玩（键盘当三键）
make play-check        # 试玩窗口的无头自检
make bench-teams       # 多队伍 AI 对战基准（--quick / --games N / --csv PATH）
make check-consistency # 只跑跨文件一致性检查
make screen-preview    # 导出固件渲染层画的那几屏图（验收用）
make render-compare    # 战斗界面与 PC 预览逐像素比对
make stat-golden       # 改了 stat_formula.py 后重新生成 golden 头文件
make lvgl-font FONT=data/fonts/SmileSans-Oblique.ttf   # 重新生成中文字库
```

---

## 5. 权威数据源与防复发体系 ★

**这是项目最有价值的资产，也是踩坑最多的地方。** 项目里踩过三次同一类坑：
**「手写数据/公式 + 没有上游交叉校验 + 测试期望值照着实现写」**——三次都让测试全绿却是错的。

三次分别是：属性克制表（14 格错）→ 能力值公式（全游戏偏移）→ 能力等级倍率表（13 档错 11 档）。

**因此形成的铁律：任何一个数值，都必须有一个游戏外可查的权威来源 + 生成器 + 逐字节一致性检查。
要让错误溜过去，得同时错三处。**

### 5.1 权威来源清单

| 概念 | 唯一权威 | 固化机制 |
|---|---|---|
| 属性克制（324 格） | `data/csv/type_efficacy.csv` + `tools/type_chart.py` | `check_consistency.py` 第 15 组全量比对 |
| 能力值 / 等级倍率 / 性格 | `tools/stat_formula.py` | 生成 `gen/poke_stat_golden.h`，`test_calc` 引用；第 17 组校验 |
| 中文名 | `tools/zh_names.py` `load_names_by_en()` | **按英文名配对**，与两侧 id 顺序无关 |
| Mega 形态 | `MEGA_WHITELIST` 硬编码白名单（41 形态 / 39 基础种） | 不能用 identifier 含 `-mega`（Veekun 混入 11 个未实装占位） |
| Mega 石 | `MEGA_STONE_NAMES` 白名单（39 块） | 不能用 `ite` 后缀匹配 |

### 5.2 外部已知值回归（`stat_formula.py` 的 `KNOWN_STATS`）

用**游戏外能查证**的实数值当锚点，让参考实现自检：
巨金怪 405 攻、暴飞龙 328 速、耿鬼 350 速、幸福蛋 714 血、班基拉斯 404 血……

`self_check()` 断言参考实现能重算出它们；`test_calc` 新增 47 项断言（9 组已知值六项全比 +
13 档 ×2 组倍率表 + 负档必须真降 + 等级真的改变伤害）。

> 写 golden 时自己踩过的两个坑（都被机制抓出来了）：
> ① 把 **#445 烈咬陆鲨**写进已知值——本表只到三代 1..386，`base_stat_of` 找不到就一律返回 50
> （「有值但全错」）；② **巨金怪 HP 种族值 70 写成 80**。

### 5.3 关键数据约定（硬规矩）

- 属性枚举按 Veekun `type_id` 顺序（1=Normal … 18=Fairy），索引 = type_id − 1，**不要按名字排序**。
- 招式 id 用 Veekun `moves.csv` 编号（id=1 是冲撞 Pound），**一律用 `mv_get(id)` 访问，
  不要写裸下标**（曾因错位让 280/353 个招式装配成下一个招式）。
- 能力等级倍率**不要写查表**，一律走 `calc_stage_ratio()`（`base=2` 能力值 / `base=3` 命中闪避；
  负档真的降、正档真的升，负档截断取整：-1 档 100→66 不是 67）。
- 性格的「−特攻」与「−特防」不是一回事：`spa=4`、`spd=5`。
- 中文名按**英文名**配对，不要按 Veekun 的 id（两边 id 顺序不同）。
- 精灵图用 `sprite_get(pid, kind)`，`kSpriteTable` 下标与 pid 无关；
  4bpp 解包 `data[i/2] >> ((i%2) ? 0 : 4) & 0xF`，**生成端与消费端必须同一条公式**。
- 六张数据表都有 `name`（英文，**代码查找键**）与 `name_cn`（中文，**只给 UI 显示**）。
- 字库按码点二分查找，`kFontMap` **必须保持升序**；字格 14 宽不是 8 的倍数，
  行字节数一律用 `FONT_STRIDE`，**不要写 `FONT_CELL_W / 8`**（整数除法丢像素）。
- 改数值请改 `stat_formula.py`，然后 `make stat-golden`。

---

## 6. 踩坑记录精选（最容易复发的）

按「下次很可能再踩」排序，完整版见 `README.md` 的踩坑记录章节。

1. **测试期望值不能照实现写。** 三次严重错误全出自这里：`test_calc.c` 的注释里就写着错公式
   算出来的 `(49+7)*2*50/100+5=61`。**期望值必须来自外部权威**。
2. **同一概念两处实现**必然漂移。命中/闪避等级曾有一套「正确的内联公式」与「错误的 `apply_stage` 表」
   并存——已收敛到 `calc_stage_ratio()`。新基准要用 `play_game` 时也是先抽 `test/ai_arena.h`，
   而不是再抄一份（否则两边胜率立刻不可比）。
3. **手写映射表 + 下标顺序**是重灾区：克制表 14 格错、性格中文名 25 个错 24 个（Veekun `nature_id`
   顺序与本项目不同）、`kMoves[]` 索引错位。凡是查表，先问「这个表从哪来的」。
4. **测试会悄悄失效。** `test_ai` 的「档位差异」原来每轮 `rng_reset()`，ROOKIE 的 15% 失手在 60 轮里
   读到同一个数，断言退化成「L1 vs 搜索」——改能力值后才暴露。改测试基础设施时要回头验证老断言是否还有效。
5. **数据源的坑**（详见 `docs/data-sources.md`）：跨世代数据污染、招式效果数据、中文名只有繁体、
   特性/道具效果没有机读源、精灵图位深 bug。
6. **字库是「安静地错」的**：代码后写、字库先生成 → 缺字画成空白，必须靠 `dump_screen` 导图肉眼验收，
   或靠 `check_charset()` 主动报错。曾缺 6 个字（主 菜 队 伍 配 败）。
7. **渲染层的坑**：`status_right[24]` 太小把提示截断在半个汉字上（已改 `SES_STATUS_TEXT_MAX 48`）；
   `ses_view()` 漏挂指针导致等级/HP 数字凭空消失而测试全绿。
8. **Windows 传中文参数**有坑（详见日志）；`check_consistency.py` 找二进制要按 `.exe` 兼容。

---

## 7. 路线图与待办

### 7.1 下一步（按依赖排序）

| # | 事项 | 说明 |
|---|---|---|
| 6 | **队伍配置**（或挪到微信小程序） | 386 只的图鉴**列表**已能画能选（菜单绘制已完成），缺的只是「选中之后改写这只的性格 / 努力值 / 个体值 / 道具 / 招式」。 |
| 7 | **NVS 存档** | 结算画面已随菜单绘制完成。烧录注意：从 `0x0` 烧合并 `full.bin` **会重置 NVS**，要保数据得用分段 `idf.py flash`。 |
| 8 | **接入官方 BSP** | 屏幕 / 按键 / 音频初始化改用 `components/bsp`；渲染层可直接复用（只要求「一块能写的 RGB565 内存」）。 |

### 7.2 已知待定（需要拍板）

- **AI 难度档位数量不一致**：需求写 5 档（简单 / 普通 / 困难 / 大师 / 大师+），
  `src/poke_ai.h` 目前只有 3 档。且基准显示三档强度无差别（见 §2.3）。
- **字体在 16px 下偏细**：得意黑是倾斜标题体、只有一个字重，16px 下笔画约 1px，4bpp 抗锯齿后发虚。
  若走 LVGL 路线，建议再生成一份常规直立字体（如 Noto Sans SC）对比。
- **1bpp 字库缺半角 ASCII**，会影响战斗界面 HP 数字排版。
- **基准的 AI-vs-AI 随机流耦合**（见 §2.3 第 3 条）：要不要修 `ask()` 的 save/restore，待定。
- **真机随机种子**：`s_rng` 默认固定种子 `0x9E3779B9`，真机若不重播种（`esp_random()` / RTC），
  每台设备的随机序列完全一致。属于固件阶段要做的事。

---

## 8. 多队伍基准这一轮（2026-10-14 已提交）

「多队伍 AI 对战基准」这一轮共 **7 处**改动（此前记为 5 处，漏了 README 数字同步与本文档）：

```
 M .gitignore          （新增 data/ai_bench/ 忽略规则）
 M Makefile            （新增 bench-teams 目标）
 M README.md           （AI 胜率 70%→68%、一致性 16→17 组等数字同步）
 M test/test_ai.c      （改为 include 共用脚手架 ai_arena.h，删掉本地副本）
 A test/ai_arena.h     （新：与 test_ai 共用的对局脚手架）
 A test/bench_teams.c  （新：多队伍基准程序）
 A WORKLOG.md          （新：本工作记忆）
```

- 重构后 `test_ai` 行为完全一致（仍 136/200 = 68%），`make test` **951 项全绿**，零警告。
- `data/ai_bench/results.csv` 是基准产出（已按 `.gitignore` 忽略）。
- 同批还修了 `ai_choose` 兜底块的缩进/损坏注释，并让 `arena_play` 的 `bad_out` 可传 NULL。

---

## 9. 五支预设队伍与「选择队伍」界面（2026-10-14 本轮）

本轮目标：给玩家 **5 支主流预设队**（性格 + 努力值 + 个体值 + 道具全套），
主菜单新增「选择队伍」入口，对手从 5 支预设队里随机抽；顺带接通引擎里遗留的道具死钩子。

预设队（`src/poke_session.c` 的 `kPreset[UI_PRESET_COUNT][TEAM_SIZE]`，均为 Lv50 / 6v6）：

| # | 队名 | 思路 |
|---|---|---|
| 1 | 晴天队 | 九尾（日照）开天气 → 输出手收尾 |
| 2 | 雨天队 | 大嘴鸥（降雨）开天气 → 水系特攻 |
| 3 | 威吓物攻队 | 威吓压制 + 物理强攻 |
| 4 | 高速特攻队 | 先手速度线 + 特攻炮台 |
| 5 | 肉盾消耗队 | 高耐久 + 消耗 / 回复 |

改动清单（22 个文件）：

```
 M src/poke_session.c/h  （SesMonSpec 扩展 + 5 队表 + 随机对手 + 选队动作）
 M src/poke_ui_state.c/h （UI_SCREEN_PRESET / 主菜单三项 / UI_ACT_PICK_TEAM / UI_PRESET_COUNT）
 M src/poke_item.c/h     （接通道具钩子：能力固定翻倍 / 免疫 / 先制之爪 / 反伤）
 M src/poke_calc.c       （道具参与能力值计算）
 M src/poke_battle.c     （道具钩子接入战斗回合）
 M test/test_session.c   （预设队校验器 + 选队后开打 + 修 test_mega / test_device_path）
 M test/test_ui.c        （选队界面自检）
 M test/test_battle.c    （道具钩子断言）
 M test/play_pc.c        （selfcheck 补一次主菜单导航）
 M test/dump_screen.c    （新增 preset 屏 + 导航适配）
 M tools/check_consistency.py（解析 #define / enum 符号常量）
 M gen/font_chars.txt + gen/font_app_12.c + gen/font_app_16.c
                         + gen/poke_font_data.c/h（补「肉」「耗」并重生成 LVGL 字体）
 M Makefile              （SCRIPTS_SCREENS 加 preset；注释「七屏」→「八屏」）
 M README.md / WORKLOG.md（断言计数 951 → 983 同步）
```

**根因与教训**：

1. **kPreset 四只特性数值笔误**：九尾 `66`、圈圈熊 `59`、巨钳螳螂 `104`、大力鳄 `63`
   —— 都不是对应特性（应为 `70` DROUGHT / `62` GUTS / `101` TECHNICIAN / `67` TORRENT）。
   用 `gen/poke_abilities.c` 权威核对逐个排查抓出。**教训：`AbilityKind` 枚举值 == Veekun ability id，
   配队数据必须按 id 逐个核，不能凭记忆写。**
2. **主菜单 2 项 → 3 项，打翻一批「光标 0 = 开始对战」的旧断言**：`test_device_path` 与
   `play_pc` 的 `selfcheck` 都假设开局光标停在「开始对战」，加「选择队伍」后按住确定只会开选队屏。
   修法都是在开局前补一次「下」。**教训：默认光标位置是界面契约的一部分，改菜单顺序要全仓搜依赖它的断言。**
3. **新屏文案带出 2 个缺字**：队5「肉盾消耗队」用到「肉」(U+8089)、「耗」(U+8017)，
   `dump_screen` 全屏覆盖校验（problems=0）抓出；补进 `gen/font_chars.txt` 后重生成 LVGL 字体。
4. **一致性检查误报**：第 16 组原来只认数字 `case`，而 `ui_screen_rows()` 现在返回
   `UI_MAIN_COUNT` / `UI_PRESET_COUNT` 这类符号常量 → 给 checker 加 `#define` / `enum` 解析。
5. **`test_mega` 前提变了**：默认首发由喷火龙变成九尾，而 Charizardite X 的 Mega 石与九尾物种不匹配
   —— 改成把 0 号位换成喷火龙再测。

**验证**：`make test` → 983 项全过 / 17 组 0 处不一致 / 零警告；
`make play-check` → 15/15；`dump_screen` 八屏覆盖校验 problems=0。

---

## 10. 日志自动推进 / 战斗 HUD / 32×32 队伍小图标（2026-10-14 本轮）

本轮目标：把「开场日志一条条自动放」做成**受玩家控制**的自动播放，给战斗界面
补 **天气标志 + 我方道具标志**，再把新做的 **386 只 32×32 队伍小图标**接进
选择队伍 / 队伍配置 / 换人三屏（换人页顺带挂上 HP 条）。

**行为**：

- **日志受控自动推进**：战斗开始后逐条播放，普通行停 `SES_LOG_DWELL_MS`=1 秒，
  关键行（日志含「天气/特性/起作用/威吓/倒下/濒死」之一）停 `SES_LOG_DWELL_KEY_MS`=1.5 秒。
  按住任意键 = 暂停、松开 = 恢复；短按确定 = 前进一条；长按确定 = **取消自动播放并跳到本批末尾**。
- **状态行**：等待手动推进时左侧画旋转动效（四帧 `/ - \ |`，每帧 `SES_WAIT_ANIM_MS`=125 ms）；
  右侧文案自动播放中显示「自动中…」，等待推进时显示「确定   长按跳过」。
- **天气胶囊**：右上角右对齐贴边，文字「晴/雨/沙/雹」；**无天气不画**。
- **我方道具名**：**纯文字、无图标**，画在血条框 HP 数字行左侧（如「木炭」）；**无道具不画**。
- **换人页**：每行「行首 32×32 图标 + 名字 + LvNN 小字 + 右侧 HP 条 + `cur/max`」；
  HP 条分档绿/黄/红（与战斗界面同一套），行高用 `MENU_ROW_H_ICON`=37。
- **队伍配置（图鉴列表）一屏行数**由 8 改为 **6**。

**关键参数**：

| 常量 | 值 | 含义 |
|---|---|---|
| `SES_LOG_DWELL_MS` | 1000 | 普通日志行的自动停留时长（ms） |
| `SES_LOG_DWELL_KEY_MS` | 1500 | 关键日志行多停一拍（天气/特性/起作用/威吓/倒下/濒死） |
| `SES_WAIT_ANIM_MS` | 125 | 等待推进动效每帧毫秒（4 帧循环） |
| `SES_LOG_AT_NONE` | `0xFFFFFFFF` | `log_at_for` 的「未计时」哨兵 |
| `MENU_ROW_H_ICON` | 37 | 带图标的列表行高（默认行高 28） |
| `ICON_COUNT` / `ICON_SIZE` | 386 / 32 | 图标表张数与每张尺寸 |
| `MY_ITEM_X` / `MY_ITEM_DY` | `HB_ME_X+8`（=118）/ `HB_NUM_DY`（=28） | 我方道具名的 x / y 相对坐标 |
| `WX_BADGE_Y`/`H`/`PAD_X`/`R`/`RIGHT` | 4 / 16 / 6 / 5 / 4 | 天气胶囊几何（右对齐贴边） |

**改动文件清单**（24 处）：

```
 M .gitignore            （新增 data/icons_4bpp、data/icons_raw 忽略规则）
 M Makefile              （新增 icons 目标；SCREEN_SRC 加入 gen/poke_icons.c）
 M src/poke_session.c/h  （受控自动推进 + key_down 维护 + 换人页图标/HP/等级/cur-max + 行高）
 M src/poke_ui.c/h       （wx_badge 天气胶囊 + 我方道具名 + 行首图标/HP 条/子行 + 等待动效 + MENU_ROW_H_ICON）
 M src/poke_ui_state.c   （图鉴列表一屏行数 8 → 6）
 M test/dump_screen.c    （build_hero 补 wx_text="晴" / my_item="木炭" / wait_anim=0）
 M test/play_pc.c        （自检适配新的日志推进节奏）
 M test/test_render.c    （HUD 天气/道具/换人条的取点断言）
 M test/test_session.c   （自动推进时序 + 换人页映射断言）
 M test/test_sprites.c   （图标集自检：张数/尺寸/覆盖率）
 M test/test_ui.c        （行数断言 8 → 6）
 M tools/make_readme_images.py（补画天气胶囊与我方道具名）
 M README.md / WORKLOG.md（断言计数 983 → 1065 + 图标/天气/道具/换人说明）
 M docs/img/hero.png     （重出，无损重生成）
 A tools/build_icons_c.py（新：32×32 图标生成器）
 A gen/poke_icons.c/.h   （新：386 张 32×32 图标，已入库）
```

**根因与教训**：

1. **「无按键」判定不能用 `key_is_down()`。** 单测直接投按键事件、**不走 `key_poll`**，
   于是 `key_is_down()` 永远读不到按下状态，暂停/恢复形同虚设。改成在会话层用
   `key_down` 布尔量，由 `ses_feed` 依 `KEY_EV_PRESS` / `KEY_EV_RELEASE` 自行维护。
   **教训：能被单测直投的层，别去依赖「只在上层轮询里才有意义」的状态查询函数。**
2. **计时惰性初始化，避免与合法序号 0 撞车。** `Session` 整体清零后 `log_at_for` 会变成 0，
   而 0 是一个**合法日志序号** —— 不额外设哨兵就会被误判成「本条已在计时」。
   改用 `SES_LOG_AT_NONE = 0xFFFFFFFF`，并在清零时补设。
   **教训：「0 合法 + 又是默认初值」的计数字段，必须另设哨兵。**
3. **换人页图标的 pid 必须用 `base.species_id`，不能用 `active_pid`。** Mega 形态的
   pid ≥ 10000，而图标表只覆盖 1..386，用 `active_pid` 会查不到图标（静默空白）。
   **教训：查「按图鉴号索引」的静态表时，一律用基础物种号。**
4. **`row_hp_permil` 用「> 0 才画」，而不是 -1 哨兵。** 存活宝可梦的 permil 必然 ≥ 1，
   0 天然是「无血条」的哨兵，不必引入 -1（引入就得在每处初始化时记得写 -1）。
   **教训：哨兵尽量选「数据域里不可能出现的值」，省掉一片初始化。**
5. **队伍配置一屏行数 8 → 6 是受约束的结果。** 带上 32×32 图标后行高由 28 变 37，
   8 行的列表板会**压到状态行**，触碰「列表板不得压状态行」的编译期自检断言，
   故降到 6 行。**教训：加行内元素时要回头看它对「一屏几行」和下层几何的连锁影响。**

**验证**：`make test` → **1065 项断言全过 / 17 组 0 处不一致 / 零编译警告**；
`make play-check` → 15/15；`make render-compare` → 差异 >32 的像素 **0.00%**（阈值 2%）通过。

---

## 11. 环境事实速查（硬件侧）

来自官方仓 `github.com/FoloToy/ai-passport`（MIT）的**已查证事实**，早期版本全估错过：

- 显示**不是**全屏帧缓冲，是 **9.6 KB 条纹缓冲**。
- 长按是 **500 ms**（不是 800 ms）。
- BLE 传队伍用 **行分隔 JSON over NUS**（不是二进制帧）。
- 官方内置中文字体**不可用**（覆盖实测），需自带字体。
- 官方已有可复用的东西：LVGL、`components/bsp`、微信小程序（但小程序不能用于配队）。
- Flash / NVS 细节与烧录注意事项见 `README.md`。

---

*本文档由开发过程中的工作记忆汇总而成。新产生的决策、踩坑、进度请继续追加，保持「下次接手 5 分钟能进入状态」。*
