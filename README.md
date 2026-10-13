# poke-battle

**在 FoloToy AI Passport 上跑的宝可梦对战引擎。** 第六代能力值与伤害公式 + 三代图鉴
（386 只 + 41 个 Mega 形态），纯 C11，**无动态内存、无 I/O**，因此能在 PC 上把一整局
6v6 跑完并断言每一步的结果。

<p align="center">
  <img src="docs/img/hero.png" width="330" alt="战斗界面布局示意">
</p>

<p align="center"><sub>
界面布局示意（240×320 原生分辨率 ×3）。<b>渲染层还没写，这不是真机截屏</b>——
图里的精灵图与文字都是把固件数据解出来摆到规划位置上生成的，见
<code>tools/make_readme_images.py</code>。
</sub></p>

| | |
|---|---|
| 目标平台 | FoloToy AI Passport：ESP32-C3，8 MB Flash，**无 PSRAM**，240×320 屏，3 个实体键 |
| Flash 占用 | ~1.03 MB / 8 MB（13%），其中精灵图 867 KB |
| 数据规模 | 386 种族 + 41 Mega / 354 招式 / 251 特性 / 383 道具 / 25 性格 |
| 测试 | **324 项断言全过，零编译警告**（`make test`，约 30 秒） |
| AI 强度 | 200 局 6v6 对随机手 **70%**，同批种子随机基线 52% |
| 进度 | 数据层 + 规则层 + AI 完成；**UI / 渲染层未开始** |
| 许可 | MIT（仅覆盖本项目代码；精灵图与字库是第三方衍生物，见[许可](#许可)） |

```bash
git clone https://github.com/gefeimu/poke-battle.git && cd poke-battle
make test            # 编译并跑全部测试
make bench           # AI 决策耗时剖析
make sprites-preview # 把固件里的精灵图导成 PNG 看画质
```

克隆下来即可构建：`gen/` 里的生成产物已入库（源数据 14 MB 见
[数据来源与重建方法](docs/data-sources.md)）。

> 自用学习项目。宝可梦相关设计版权归任天堂 / Creatures / GAME FREAK 所有。
> 代码采用 [MIT 许可](#许可)；第三方数据与字体不受 MIT 覆盖，见[许可](#许可)。

---

## 目标需求

### 对战规则

| 项目 | 要求 |
|---|---|
| 模式 | 本地单机对战 |
| 队伍 | 6v6，完整对局 |
| 等级 | 默认 50 |
| 换人 | 支持，含行动顺序与Mega 触发时机 |
| 道具 | 可携带，战斗中使用 |
| 队伍配置 | 玩家自选 6 只 vs 6 只随机，或对战预置强队 |
| 个体值 | 努力值可调（6 项 × 0~255） |
| 性格 | 可调（25 种，±10%修正） |
| Mega 进化 | 支持（52 个形态，仅限三代宝可梦） |
| 精灵图 | 火红/叶绿版 64×64 正面立绘 + Mega（统一 64×64） |
| 中文显示 | 两套可选：自制 14×14 点阵（35 KB）或 LVGL 16px 4bpp 抗锯齿（119 KB），均 1274 字 |

### AI 要求（核心）

**AI 不能太笨。** 这是整个项目的重点，需要能达到"练习对战"的价值：

- 会算属性克制，不会用克制招打不划算的局面
- 懂换人时机（对手上了克我的，要能换掉）
- 会评估净收益（克制但只有 30% 命中且会暴击被反杀，就不该用）
- 懂资源管理（PP 够不够、要不要留收尾的）
- 分难度等级：简单 / 普通 / 困难 / 大师 / 大师+

算力已验证充足：ESP32-C3 单次伤害计算约 0.0006 ms，5 ms 预算可搜索 8888 个节点，足够 4 层以上完整前瞻。**瓶颈不在算力，在评估函数质量。**

### 无现成 AI 可用

Pokémon Showdown 的 `sim/bots/` 已从 master 移除，只剩 `RandomPlayerAI`（纯随机）。第三方 AI（Technical Machine 等）都是 TypeScript/Python 且依赖 PS 引擎做前向模拟，无法移植到 8 MB Flash。

**可借鉴的是算法而非代码**：Technical Machine 的状态评估权重表（纯数学，可翻成 C）、Showdown 的招式与克制数据。

---

## 硬件约束

| 资源 | 总量 | 本项目占用 |
|---|---|---|
| Flash | 8 MB | ~1.01 MB（12.6%） |
| SRAM | ~380 KB 可用 | ~75 KB（20%） |
| 屏幕 | 240×320 RGB565 | 上半屏 240×160 战斗场景 + 下半屏菜单 |
| 按键 | 3 个（▲▼ OK） | 上下移动光标 / 短按确认 / 长按返回 |
| 电池 | 500 mAh | 纯单机，不联网 |

屏幕 240×320 正好是 GBA 原生 240×160 的两倍，可上下分屏还原初代版式。

**关键优化**：不联网可省下 60~80 KB SRAM（Wi-Fi 协议栈）。

---

## 界面设计（待实现）

<p align="center">
  <img src="docs/img/hero.png" width="300" alt="战斗界面布局">
</p>

<p align="center"><sub>版式、框体尺寸与配色全部取自三代原版（火红/叶绿），不是自己设计的。
图由 <code>make readme-images</code> 从固件数据生成。</sub></p>

### 版式：照三代原版摆

屏幕 240×320 正好是 GBA 原生 240×160 的两倍，于是：

| 屏区 | 内容 |
|---|---|
| y = 0–159 | **完整复刻原版战斗画面**：双方立绘 + 两个血条框 |
| y = 160–319 | 对话框（战斗日志）+ 招式菜单 + 状态行 |

原版把对话框压在画面底部那 48 px 里，血条框和对话框是挤在一起的；我们高一倍，
正好可以把对话框整块挪到下半屏，两个血条框都完整可见。

版式数值是**从反编译仓查出来的，不是估的**（`pret/pokefirered`）：

| 项 | 原版值 | 出处 |
|---|---|---|
| 对手血条框 | **128×32**，左上角贴边 `(0,0)` | OAM 是 `SPRITE_SHAPE(64x64)` 的**两个**精灵拼合 → 宽 128 |
| 我方血条框 | **128×64**，右下角贴边 `(112,88)` | 同上；多一行 HP 数字 + 一条 EXP 条 |
| 摆放 | 左上 / 右下**对角**，各占半屏宽 | 原版即如此 |
| 框体 | 米白填充 `#FFFFDE` + 深灰描边 `#414141` + 右下墨绿投影 `#526A62` | `graphics/battle_interface/healthbox.pal` |
| HP 条 | **双色立体**：上行高光 + 下行基色，外套深色槽 | `healthbar.pal` |
| HP 条三档色 | 绿 `#73FFAC`/`#5AD583`、黄 `#FFE639`/`#CDAC08`、红 `#FF5A39`/`#AC414A` | 同上 |
| 对话框 | 白底 + 深蓝外框 `#414A7B` + 浅蓝内框 `#6A8BBD` | `textbox1.pal` |
| 框内文字色 | 深绿 `#203900` | `healthbox.pal` |

「双色立体」这条值得单说：原版的 HP 条**不是单色矩形**，而是上行高光、下行基色
的两段式，外面再套一圈深色槽。按单色画会明显「不像」，所以 `hp_bar()` 按这个实现。

### 两个字体相关的真实缺口

做这版界面时暴露出来的问题，都不是排版能绕过的：

1. **我们的字对血条框偏大。** 原版血条框里的字约 8 px 高；我们的选项最小是
   14×14（自制点阵），LVGL 那套 16 px。同样的框里我们的字明显更大更挤。
   可选做法：再生成一套 12 px 的 LVGL 字体专供血条框（对话框仍用 16 px，
   多占约 50 KB Flash），或者接受现状当成「等比放大版」。
2. **自制 1bpp 字库的 ASCII 是全角等宽。** 每个字母占满 14 px，`68/153` 需要
   126 px，**塞不进 128 px 的血条框**。要做 GBA 风格界面，必须给 1bpp 字库
   补一套**半角 ASCII**（7 px 宽），否则 HP 数字和等级都没法排。

第 2 条只影响自制字库；LVGL 那套的 ASCII 本来就是比例宽度（见
[中文字库](#中文字库)），所以现在先用它出图。

### 要写的模块

| 文件 | 职责 | 关键点 |
|---|---|---|
| `src/poke_ui.c/.h` | 界面状态机 | 4 组界面的切换、光标移动、把按键翻译成引擎动作 |
| `src/poke_input.c/.h` | 三键驱动 | 自己做去抖与短按/长按判定（官方 BSP 只给 ADC 读数） |
| `src/poke_save.c/.h` | 队伍配置持久化 | NVS；24 KB 分区约可存 100 支队 |
| `main/app_main.c` | ESP-IDF 入口 | 屏幕/按键初始化、lifecycle 任务 |
| `test/test_ui.c` | 状态机单测 | 喂按键序列 → 断言状态迁移与 `b->my_action` 取值 |
| `test/dump_screen.c` | 帧缓冲导出 PGM | **不上硬件就能验证布局** |

**绘制原语不用自己写**——官方 BSP 已提供显示与按键，绘制交给 LVGL。
这条是查过官方仓之后才确认的：最初的设计里我把「屏幕驱动 + 绘制原语 +
中文字体」都算成了要自己实现的工作量，实际只需要写界面逻辑。

### 4 组界面

| 界面 | 内容 |
|---|---|
| **队伍配置** | 386 只列表（可滚动/检索）+ 逐只编辑性格(25)/努力值(6×0-255)/个体值/道具(383)/4 个招式(354)，每个都是可滚动选择器；再加难度选择。**工作量最大的一块** |
| **战斗主界面** | 立绘走 `sprite_get(pid, SPR_FRONT)`；名字/等级/HP 从 `battle_active_c(b, side)` 读 `cur_hp`/`hp_max`；招式菜单用 `mv_get(id)->name_cn` + `m->pp[i]` |
| **战斗中弹层** | 换人 / 背包 / Mega 确认 / 逃跑。Mega 走 `battle_mega()`，**不消耗回合，必须在出招前调用** |
| **日志与结算** | 日志逐条播放；胜负画面读 `b->result` |

### 4 个必须处理的引擎接口约束

| 约束 | 处理方式 |
|---|---|
| `battle_step()` 返回 0/1/2/3 | 1 = 我方需换人，2 = 对方，3 = 双方。**返回值非 0 时必须先进强制换人界面**，不能直接进下一回合 |
| 出招与换人互斥 | 出招填 `b->my_action`（0..3）；换人填 `my_action = 0xFF` + `my_switch`。**换人消耗回合，双方都不出招** |
| 日志有开关 | 跑 AI 自对弈时可 `battle_log_enable(false)` 省掉 `vsnprintf` 开销；正式对局要开着 |
| 随机源统一 | 不能自己调 `rand()`，引擎的随机全部走 `battle_rng()`（这样单测才能固定种子重放） |

### 硬件侧三条事实（来自官方仓，我最初全估错了）

| 项 | 实际 | 我最初的估计 |
|---|---|---|
| 显示缓冲 | LVGL **部分刷新**，一个 `240×20` RGB565 ≈ **9.6 KB** | 以为要 150 KB 全屏帧缓冲，还担心放不下双缓冲 |
| 长按阈值 | **500 ms**（短按 180 ms） | 以为 800 ms |
| 按键接线 | 三键**共用 GPIO0**，靠电阻分压区分：上 0 mV / 下 300 mV / 确定 595 mV / 松开 3300 mV | 以为是三个独立 IO |

按键还有个具体的坑：**ADC 读失败必须当「未按下」，不能当 0 mV**，否则一次读错
就会被判成「上键被按住」。另外**不能用内部上拉**（45 kΩ 会把三档全挤进 0~154 mV）。
官方 BSP 里长按阈值特意从组件默认的 1500 ms 下调到 500 ms，理由是「在这台三键小设备上
明显偏迟钝」。

### 队伍配置可以挪到配套小程序

「队伍配置」是设备端最大的一块（386 项列表 + 7 个选择器）。替代方案是做成微信小程序、
用 BLE 把队伍推给设备，设备端只留「选第几队 → 对战 → 结算」。

配置数据量很小，传输毫无压力：

| 字段 | 字节 |
|---|---|
| 种族 id 2 + 等级 1 + 性格 1 + 特性 1 + 道具 2 | 7 |
| 4 个招式槽 4×2 | 8 |
| 努力值 6 + 个体值 6 | 12 |
| **单只合计** | **27** |

一队 6 只 = 162 字节，加头部（版本/序号/CRC16）约 **170 字节**。

唯一要留心的约束是 **BLE 默认 MTU 23 字节，扣掉 ATT 头只剩 20 字节有效载荷**；
`wx.setBLEMTU` 只在 Android 5.1+ 生效、**iOS 不支持**（调了不报错但不生效），
所以必须按 20 字节分包：170 字节 = 9 包，每包带序号 + 总包数 + 数据。
BLE 会丢包，收齐后要用头部 CRC 校验，失败要求重传。

**状态：仅讨论，未实现。** 无论是否做小程序，固件都应内置 1~2 支**只读预设队伍**——
否则「刚焊好的板子」和「手机不在身边」两种情况下设备完全没法玩。

---

## 当前进度

| 模块 | 状态 | 说明 |
|---|---|---|
| 三代数据表 | ✅ 完成 | 91 KB，含中文名，83 项自检断言全通过 |
| 精灵图 | ✅ 完成 | 427 张 4bpp（正面 386 + Mega 41），867 KB，14 项自检 |
| 能力值与伤害公式 | ✅ 完成 | 15 KB，83 项单测全通过 |
| 对战引擎（回合流程） | ✅ 完成 | 30 KB，100 项单测全通过 |
| 招式效果分派 | ✅ 完成 | 能力变化/异常/回复/吸取/多段/畏缩/天气 |
| 特性系统 | ✅ 完成 | 251 个特性，26 项单测（出场/回合末/免疫/倍率） |
| 道具系统 | ✅ 完成 | 16 属性增强/吃剩的/焦点镜片/贝壳之铃等，21 项单测 |
| 对战 AI | ✅ 完成 | L1 启发式 + 2-ply + 蒙特卡洛，19 项单测 |
| 中文字库 | ✅ 完成 | 1274 字，两套方案（1bpp 35 KB / LVGL 4bpp 119 KB），25 项自检 |
| UI / 渲染 | ⏳ 未开始 | 三键状态机 + 精灵图绘制，见[界面设计](#界面设计待实现) |

测试合计 **324 项断言全部通过，零编译警告**。用 `make test` 一键运行。
另有跨文件一致性检查（`make check-consistency`），当前 0 处不一致。

已完成的核心能力（`src/poke_calc.c`）：

- 六项能力值（含 HP 特殊公式、性格 ±10%、努力值）
- 属性克制（18×18 矩阵 + 双属性相乘）
- 完整伤害链：BaseDamage → 天气 → STAB → 克制 → 随机 → 暴击 → 灼伤
- Mega 形态切换（保留等级/性格/努力值，仅换种族值与属性）
- 抽签源抽成独立函数，便于单测固定 seed

### 对战 AI（`src/poke_ai.c`）

目标是不做「太笨的电脑」。分三层：

- **L1 启发式**：对每个候选（4 个招式槽 + 最多 5 次换人 + Mega 变体）打分。
  计入期望伤害、克制倍率、STAB、能力等级、命中率、先制、异常/回复/吸取、
  PP 消耗、对手本回合最大威胁、免疫、尚未实现的招式直接重罚。
- **L2 2-ply**：对 L1 前 N 个自身候选，枚举对手前 K 种应手，在克隆状态上
  真跑 `battle_step`，按「对手威胁权重」加权平均。同时出手、换人时机、
  先制抢血都在这一层体现。**注意是加权期望而非 min**——min 是「防最坏
  情况」，会让 AI 永远挑不会被秒的招，实测胜率反而从 70% 掉到 48%。
- **L3 蒙特卡洛**：对 L2 前 3 名各跑若干局到终局，吸收命中率、
  连续命中、异常触发等方差。
- **融合方式：搜索是修正项，不是替代项。** L1 分是 1e4 量级的战术判断，
  搜索分是 1e5~1e6 量级的物资评估，**直接用搜索分覆盖 L1 会让只看
  HP/种族值的 eval 主导决策**，实测 73% → 47%。所以搜索分先归一化到
  [-1,1] 再乘小权重加到 L1 上（`norm * 200`）。

实测强度（`test/test_ai.c`，200 局 6v6，AI 控 side0，对手纯随机）：

| | 胜场 | 胜率 |
|---|---|---|
| AI（冠军档，L1+L2+L3） | 139/200 | **70%** |
| 随机手基线（同批种子，随机 vs 随机） | 103/200 | 52% |

**必须带对照组**：6v6 队伍强度方差很大，只看绝对值会被误导。断言写成
「AI 胜率 − 基线胜率 >= 10 个百分点」，默认样本量 200 局（约 16 秒）。

单次决策耗时（PC，gcc -O2；ESP32-C3 约慢 20~40 倍）：

| 档位 | 节点上限 | PC | ESP32-C3 估算 |
|---|---|---|---|
| ROOKIE（仅 L1） | 0 | 0.005 ms | 0.1~0.2 ms |
| SMART（L1+L2+6 局 MC） | 220 | 0.68 ms | 14~27 ms |
| EXPERT（L1+L2+16 局 MC） | 700 | 2.0 ms | 40~80 ms |

整场 6v6 对局（AI_EXPERT 双方）PC 上约 0.08 秒，每步 3.1 ms。

---

## 目录结构

```
poke-battle/
├── src/                       引擎源码（纯 C11，无动态内存、无 I/O）
│   ├── poke_calc.c/.h         能力值与伤害公式（Gen VI）
│   ├── poke_battle.c/.h       6v6 回合流程状态机
│   ├── poke_ability.c/.h      特性效果（Veekun 无效果数据，手写）
│   ├── poke_item.c/.h         道具效果（同样手写）
│   └── poke_ai.c/.h           对战 AI（L1 启发式 + 2-ply + 蒙特卡洛）
│
├── data/
│   ├── csv/                   Veekun CSV 数据源（23 文件，11.9 MB）
│   │                           gen_gen3_data.py 的输入，勿删
│   ├── sprites_4bpp/          量化后 PNG-8 中间产物（427 张，可看画质）
│   │                           最终产物是 gen/poke_sprites.c，此目录可删
│   └── sprites_preview/       make sprites-preview 的输出（肉眼验图用，可删）
│
├── gen/                       生成产物，勿手工编辑
│   ├── poke_types.c           18 属性克制矩阵
│   ├── poke_moves.c           354 招式（含效果分派数据）
│   ├── poke_species.c         427 种族（386 默认 + 41 Mega）
│   ├── poke_learnsets.c       386 只升级学习表
│   ├── poke_natures.c         25 种性格
│   ├── poke_abilities.c       251 个特性
│   ├── poke_items.c           383 道具（含 39 块官方 Mega 石）
│   ├── poke_sprites.c/.h      427 张 4bpp 图 + 调色板 + 索引表（正面 386 + Mega 41）
│   ├── font_cn.c/.h           自制 1bpp：1274 字 14×14 点阵 + 码点映射表
│   ├── font_app_16.c          LVGL 4bpp：1274 字 16px 抗锯齿（得意黑）
│   └── font_chars.txt         字库收录的字符清单（调试用）
│
├── tools/
│   ├── gen_gen3_data.py       Veekun CSV → C 数据表（主生成器）
│   ├── regen_types.py         只重建属性克制表（改克制关系时用，不碰 CSV）
│   ├── build_sprites_c.py     精灵图 → 4bpp C 数组
│   ├── zh_names.py            Veekun 繁体中文名 → 简体（Windows API，无依赖）
│   ├── gen_font.py            中文字库子集化
│   └── fetch_sprites_local.py 逐个下载精灵图（备用，sparse checkout 更快）
│
├── test/
│   ├── data_selfcheck.c       数据表自检（78 项）
│   ├── test_calc.c            伤害公式单测（83 项）
│   ├── test_battle.c          对战引擎单测（100 项）
│   ├── test_ai.c              AI 决策质量单测（19 项，含随机基线对照）
│   ├── test_sprites.c         精灵图数据自检（14 项）
│   ├── test_font.c            中文字库自检（25 项，含全表覆盖检查）
│   ├── dump_sprites.c         把固件里的精灵图导出 PNG（肉眼验图）
│   ├── dump_font.c            把固件里的字库渲染成 PGM（肉眼验字形）
│   ├── bench_ai.c             AI 决策耗时剖析
│   └── bench_step.c           引擎单步耗时基准
│
├── Makefile                   统一构建入口（make test / bench / sprites-preview）
├── LICENSE                    MIT（只覆盖本项目代码）
└── README.md
```
---

## 常用命令

统一用 Makefile，四组测试的编译开关（`-DPOKE_BATTLE_TEST_NO_RNG`、
`-DPOKE_CALC_TEST_NO_RAND`）都收在这里，不必再手抄 gcc 命令：

```bash
make          # 编译全部
make test     # 编译并跑全部测试（约 30 秒，324 项断言 + 一致性检查）
make bench    # AI 决策耗时剖析
make types    # 重新生成属性克制表（改克制关系后必须跑）
make font-preview   # 把字库渲染成图片，肉眼检查字形
make clean

# 想用更多局数验证 AI 强度（默认 200 局约 16 秒）
AI_GAMES=1000 ./build/test_ai
```

### 各组测试的编译差异

| 测试 | 链接的源文件 | 覆盖的符号 | 断言数 |
|---|---|---|---|
| `data_selfcheck` | 仅数据表 | 无（不碰引擎） | 78 |
| `test_calc` | 全部 | `battle_rng` + `calc_rand_85_100` | 83 |
| `test_battle` | 全部 | 同上 | 100 |
| `test_ai` | 全部 + `poke_ai.c` | 同上 | 19 |
| `test_sprites` | 仅 `poke_sprites.c` + `poke_species.c` | 无（不碰引擎） | 14 |
| `test_font` | `font_cn.c` + 六张中文名表 | 无（不碰引擎） | 25 |

`calc_rand_85_100` 必须在每个链接 `poke_calc.c` 且带
`-DPOKE_CALC_TEST_NO_RAND` 的测试里提供强符号，否则链接失败。
`test_sprites` 与 `test_font` 都不链接引擎，可以独立跑。

### 重新生成数据表

```bash
# 全量重跑（依赖 data/csv，网络失败会静默截断 pokemon_moves.csv）
python tools/gen_gen3_data.py

# 只重建属性克制表（纯硬编码，不依赖 CSV，改克制关系时用这个）
python tools/regen_types.py
```

### 重新生成精灵图

先 sparse checkout 源图：

```bash
cd data
git clone --depth 1 --filter=blob:none --sparse https://github.com/PokeAPI/sprites.git
cd sprites
git sparse-checkout set sprites/pokemon/versions/generation-iii \
                       sprites/pokemon/versions/generation-v/black-white
cd ../../..
```

然后：

```bash
python tools/build_sprites_c.py \
    --src <sprites 仓库>/sprites/pokemon/versions \
    --gen3 firered-leafgreen
```

本机源图位置：`D:/Download/Chromedownload/sprites/sprites/pokemon/versions`

| 参数 | 说明 |
|---|---|
| `--gen3 firered-leafgreen` | 火红/叶绿版（默认）。原生即 64×64，无需缩放 |
| `--gen3 ruby-sapphire` | 红宝石/蓝宝石版 |
| `--gen3 emerald` | 绿宝石版（与红宝石 66% 相同） |
| `--gen3 black-white` | 五代黑白版，96×96 需缩放。与 Mega 同源但与三代正面画风不统一 |
| `--back` | 额外导入 386 张背面图（+772 KB）。**默认不导入** |
| `--shiny` | 额外导入 386 张闪亮图（+386 KB） |

生成结果：427 条目（正面 386 + Mega 41），像素数据 854 KB。

### 重新生成中文字库

```bash
# 只需一个中文字体文件（TTF/OTF），字号默认跟字格高度，一般不用改
make font FONT=C:/Windows/Fonts/Deng.ttf

# 或直接调脚本
python tools/gen_font.py --font C:/Windows/Fonts/Deng.ttf

# 生成后导图肉眼验收（必做，见下方「字库相关的坑」）
make font-preview
```

**字符来源**（`tools/gen_font.py::collect_chars`），四处汇总后去重：

1. **六张数据表的中文名**（宝可梦/招式/属性/特性/道具/性格），
   按数据表实际引用的 id 收集，不是把 CSV 全量倒进来
2. **UI 手写文案**（`UI_STRINGS` 常量）
3. **半角字符与标点**（`EXTRA_CHARS`）
4. **源码里的字符串字面量**——`gen/` **和 `src/` 都要扫**，
   战斗日志（"濒死了""第 N 回合"）在 `src/poke_battle.c` 里

改完中文名或日志文案后重跑本步骤，字库会自动跟着更新；
`test_font.c` 会验证「数据表里每个字都有字形」，漏收会立刻失败。

---

## 数据规格

### 精灵图

<p align="center">
  <img src="docs/img/sprites.png" width="620" alt="精灵图总览">
</p>

<p align="center"><sub>上图全部由 <code>gen/poke_sprites.c</code> 里的 4bpp 索引色数据解出
（<code>make sprites-preview</code>）。三代火红/叶绿立绘是原生 64×64，
Mega 形态取自五代黑白版再缩到同样的 64×64。</sub></p>

| 项 | 值 |
|---|---|
| 正面图源 | `generation-iii/firered-leafgreen`（原生即 64×64，**无需缩放**） |
| Mega 图源 | `generation-v/black-white`（96×96 → 缩放至 64×64，与正面同规格） |
| 尺寸 | 全部 64×64（`SPRITE_SIZE`） |
| 张数 | 427 = 正面 386 + Mega 41（只要正面，不含背面/闪亮） |
| 格式 | 4bpp 索引色，每字节 2 像素（4 bit/像素），高位在前，行优先 |
| 解码 | 像素 i 索引 = `(data[i/2] >> ((i%2) ? 0 : 4)) & 0xF` |
| 每图字节数 | 64 × 64 / 2 = 2048 |
| 透明 | 索引 0 = 透明，不绘制 |
| 调色板 | 每图 16 色 RGB565（`PALETTE_SIZE`） |
| Flash 占用 | 像素 854 KB + 调色板 13 KB ≈ 867 KB |

**解码公式是唯一权威**：生成端 `tools/build_sprites_c.py::pack_4bpp`
与消费端 `test/dump_sprites.c` 必须用同一条公式。曾经两边都写成 2bpp
而调色板按 16 色生成，导致图变成碎噪点——见下方踩坑记录第0 条。
改位深时**两处必须同时改**，并用 `make sprites-preview` 导图肉眼验证。

#### 精灵图事实基准（改动前先对这张表）

这个项目里精灵图的描述曾经散落在 README 顶部、README 详细段、脚本默认值、
C 文件头注释四处，四处互相矛盾且都没人发现图本身是坏的。所以把
**唯一权威**固定在这里，其余地方一律引用本表：

| 项 | 权威值 | 定义处|
|---|---|---|
| 正面图源 | `generation-iii/firered-leafgreen` | `build_sprites_c.py` 的 `--gen3` 默认值 |
| Mega 图源 | `generation-v/black-white` | 同上（写死，三代无 Mega 图） |
| 画布 | 64×64 | `gen/poke_sprites.h: SPRITE_SIZE` |
| 位深 | 4bpp（每字节 2 像素，高位在前） | `build_sprites_c.py::pack_4bpp` |
| 调色板 | 16 色 RGB565 | `gen/poke_sprites.h: PALETTE_SIZE` |
| 张数 | 427 = 正面 386 + Mega 41 | `gen/poke_sprites.h: SPRITE_COUNT` |
| 透明 | 索引 0 | `gen/poke_sprites.h: PAL_TRANSPARENT` |
| 导入内容 | 只要正面（无背面/闪亮） | `build_sprites_c.py` 默认（`--back` 才加） |
| 像素占用 | 854 KB + 调色板 13 KB | `test_sprites.c` 实测 |

**改任何一项时，这三处必须同步**：
1. `tools/build_sprites_c.py`（生成端）
2. `test/dump_sprites.c` + 未来的渲染层（消费端，解码公式）
3. 本表 + README 顶部汇总行（文档端）

改完跑两条命令验收：
```bash
make test              # 含 test_sprites 的14 项断言
make sprites-preview   # 导出 PNG 肉眼确认
```

`test_sprites.c` 专门盯「安静地错」的形态：位深退化（用到的色数 < 4）、
覆盖率异常（< 4% 或 > 85%）、张数与尺寸不符、pid 缺图。
实测它对「4bpp 被误降成 2bpp」这个 bug 是 427/427 全中。

三代目录的三个版本差异（实测 502 只，三版均存在）：

| 版本 | 与红宝石相同 | 说明 |
|---|---|---|
| `ruby-sapphire` | 基准 | 红宝石/蓝宝石 |
| `emerald` | 335 / 502（66%） | 绿宝石，与红宝石完全相同的占多数 |
| `firered-leafgreen` | 242 / 502（48%） | **167 只三版各异**，线条更干净、颜色更饱和 |

选择火红/叶绿版（`--gen3 firered-leafgreen`）。注意它不是每只都更好——皮卡丘三版完全相同，暴鲤龙改成了站姿（见仁见智）。

#### 当前实际使用：`firered-leafgreen` 版

本项目**当前用 `--gen3 firered-leafgreen`**（默认参数），正面图全部来自
`generation-iii/firered-leafgreen/{id}.png`：

- **原生就是 64×64，无需缩放**，没有 LANCZOS 缩放引入的锯齿与半透明
- 只取正面，不取背面 —— 不加 `--back` 开关
- Mega 图取自 `generation-v/black-white`（三代没有 Mega 图，Mega 是六代产物），
  96×96 缩到同样的 **64×64**，与正面图的像素规格统一
- 427 条目 = 正面 386 + Mega 41，像素数据 854 KB

想换成五代黑白版（正面改用 black-white，与 Mega 同源但画风与三代不统一）：

```bash
python tools/build_sprites_c.py --src <路径> --gen3 firered-leafgreen  # 427（当前）
python tools/build_sprites_c.py --src <路径> --gen3 black-white        # 427
```

### 中文字库

**两套方案并存**，共用同一份字符清单 `gen/font_chars.txt`（1274 字）：

<p align="center">
  <img src="docs/img/font-compare.png" width="600" alt="两套字库对比">
</p>

<p align="center"><sub>同一段文本、同一放大倍数。<b>上为自制 1bpp 点阵，下为 LVGL 4bpp 抗锯齿。</b>
注意 LVGL 版的 ASCII 是<b>比例宽度</b>（<code>Lv50</code> 只占约 4 个半角位），
自制版每个 ASCII 都占满一个 14px 全角格。</sub></p>

| | 自制 1bpp（`gen/font_cn.c`） | LVGL 4bpp（`gen/font_app_16.c`） |
|---|---|---|
| 字格 / 字号 | 14×14 | 16 px |
| 位深 | 1bpp 黑白点阵 | 4bpp 抗锯齿 |
| 每字字节 | 28（14 行 × 2 字节） | 平均约 95 |
| 收录字数 | 1274（字形槽位 1280） | 1273（字形 1283） |
| 占用 | 位图 35,840 B + 映射 5,096 B = **40,936 B（40 KB）** | 位图 **119.2 KB** |
| 源字体 | 等线 Regular | 得意黑 Smiley Sans |
| 生成 | `make font FONT=<字体>` | `make lvgl-font FONT=<字体>` |
| 谁来画 | 自己解 1bpp，`font_draw_text()` 直写 RGB565 | 交给 LVGL |
| ASCII | 等宽，每个占满全角格 | **比例宽度** |

**实测结论：得意黑（Smiley Sans）在 16px 下偏细。** 它是倾斜的标题体、只有一个字重，
16px 时笔画约 1px，4bpp 抗锯齿之后反而比 1bpp 点阵更虚（见上面对比图）。
也就是说 LVGL 方案「机制上更省事」（不用自己写渲染、字号可缩放），但**字形质量
目前不满意**，要等选定一个常规直立体（如 Noto Sans SC）再定。
现在两套都留着，切换成本只是换一个 `.c` 文件。

#### 自制 1bpp 的解码约定

**解码公式**：像素 `i = y*FONT_CELL_W + x` 的位在
`kFontBitmap[gi*FONT_CELL_H*FONT_STRIDE + y*FONT_STRIDE + x/8]`
的第 `7 - (x%8)` 位（行优先，高位在前）。

**码点映射**：`kFontMap` 按码点升序排列，`font_lookup()` 用二分查找
（**顺序是二分查找的前提**，`test_font.c` 会断言）。查不到返回 `0xFFFF`。

**渲染入口**：`font_draw_text(buf, w, h, x, y, color, utf8)` 直接写
RGB565 缓冲区，自动跳过未收录字符（不换行，由调用方排布）。

`FONT_CELL_W = 14` **不是 8 的倍数**，所以每行占 2 字节、行尾 2 位是填充。
**一律用 `FONT_STRIDE = ceil(CELL_W / 8)`，不要写 `CELL_W / 8`** —— 整数除法得 1，
会丢掉每行后 6 个像素，而读取端按 ceil 索引，两边错位后字形变成散点。

#### LVGL 4bpp 的格式约定

生成参数固定为：

```bash
lv_font_conv --font <字体> --size 16 --bpp 4 --format lvgl \
  --no-compress --no-prefilter --force-fast-kern-format \
  --range 0x20-0x7F --symbols "<gen/font_chars.txt 的内容>" -o gen/font_app_16.c
```

三个容易踩的点：

- **`--no-prefilter` 不能漏**。LVGL 默认开启逐行预滤波，解码端要做逆运算；
  我们的验证渲染器（以及未来的裸驱动）按原始 4bpp 解，漏掉它就是满屏噪声。
  官方内置字体也带这个参数。
- **字形位图是连续位打包**：一个 `box_w × box_h` 的 4bpp 字形占
  `ceil(box_w * box_h * 4 / 8)` 字节，**行与行之间不补齐字节**。
  按「每行对齐」解会全错（实测 40 个连续字形全部符合连续打包）。
- **`SPARSE_*` 才是稀疏表**。cmap 类型后缀 `TINY` / `FULL` 只表示偏移表元素的宽度
  （uint8 / uint16），**不代表稀不稀疏**。按 "TINY" 判连续会把稀疏表当连续，
  算出的字形索引远超字形总数（实测 22800 > 1721）。

`tools/render_lvgl_font.py` 能把该格式渲染成 PNG 肉眼验证，并带
「位图总长 == 各字形尺寸之和」自检。

#### 中文名从哪来

数据表里的 `name_cn` 字段（六张表都有）取自 Veekun 的
`*_names.csv` 中 `local_language_id = 4`（**繁体中文**），再用
`tools/zh_names.py` 转简体。

**Veekun 没有简体行**——它只标记了「中文（台湾）」这一种，简体与繁体
共用该行。转换用 Windows 自带的 `LCMapStringEx`，不引入
opencc / zhconv 之类的依赖：本项目要能在一台裸 Windows 上重建全部
数据表，多一个 pip 包就多一个失败点。

同存的 `name`（英文标识符）**不能删**——它是代码里的查找键
（测试用 `find_move("Flamethrower")`），和给 UI 看的 `name_cn` 是
两个用途，所以两份并存。

### Flash 占用

用 `sizeof` 实测（`.rodata`，非源文件大小）：

| 内容 | 字节 | KB |
|---|---|---|
| `poke_types` 属性克制矩阵 | 324 | 0.3 |
| `poke_moves` 354 招式（含中文名指针） | ~18,400 | 18 |
| `poke_species` 427 种族（含中文名指针） | ~15,400 | 15 |
| `poke_learnsets` 学习表 | 42,700 | 42 |
| `poke_natures` 26 性格 | ~520 | 0.5 |
| `poke_abilities` 251 特性 | ~5,000 | 5 |
| `poke_items` 383 道具 | ~16,850 | 16 |
| 中文名字符串（六表合计） | 15,088 | 14.7 |
| **数据表小计** | **~114,300** | **112** |
| `poke_sprites` 像素 427×2048 | 874,496 | 854 |
| `poke_sprites` 调色板 427×32 | 13,664 | 13 |
| **精灵图小计** | **888,160** | **867** |
| `font_cn` 位图 1280×28 | 35,840 | 35.0 |
| `font_cn` 码点映射 1274×4 | 5,096 | 5.0 |
| **字库小计（1bpp 方案）** | **40,936** | **40** |
| `src/` 引擎代码（calc+battle+ability+item+ai） | 26,160 | 26 |
| **当前合计** | **~1,083,400** | **~1,058（1.03 MB）** |

约 **1.03 MB / 8 MB = 13%**。精灵图占 82%，是唯一值得压缩的对象
（换 3bpp 或调色板复用可省 ~300 KB）。

若改用 LVGL 4bpp 字库，字库小计从 40 KB 变成约 124 KB，合计约 1.17 MB（14.6%）。

待补：UI / 渲染层（见[界面设计](#界面设计待实现)）。

### 关键数据约定

- 属性枚举按 Veekun type_id 顺序（1=Normal … 18=Fairy），索引 = type_id − 1，**不要按名字排序**
- move id 用 Veekun moves.csv 编号（id=1 是冲撞 Pound），数组 0-based 即 `kMoves[id-1]`。
  **一律用 `mv_get(id)` 访问**，不要写裸下标（曾因错位让 280/353 个招式装配成下一个招式）
- 精灵图 `kSpriteTable` 下标与 pid 无关，查图用 `sprite_get(pid, kind)`。
  像素用 `data[i/2] >> ((i%2) ? 0 : 4) & 0xF` 解 4bpp，**生成端与消费端必须同一条公式**
- 六张数据表都有 `name_cn`（简体中文显示名）与 `name`（英文标识符）。
  **`name` 是代码查找键，不要拿 `name_cn` 去做匹配**（测试里
  `find_move("Flamethrower")` 依赖英文名）；`name_cn` 只给 UI 显示用
- 字库按码点二分查找，`kFontMap` **必须保持升序**（`test_font.c` 会断言）。
  字格 14 宽不是 8 的倍数，行字节数一律用 `FONT_STRIDE`，
  **不要写 `FONT_CELL_W / 8`**（整数除法会丢像素）
- Veekun 的 `damage_class_id` 是 1/2/3（status/physical/special），本地枚举 0/1/2，生成时已做映射
- **规则世代 = 第六代（X/Y ORAS）**，由生成器 `--gen` 决定（默认 6）。宝可梦范围是三代图鉴 1..386 + 41 个 Mega 形态
- 属性按规则世代处理：`--gen 6` 直接用 Veekun 的 Gen6 值（含 Fairy，21 个种族 + 3 个招式）；`--gen 3/5` 启用 `TYPE_OVERRIDE_PRE_GEN6` / `MOVE_TYPE_OVERRIDE_PRE_GEN6` 回退。**Gen 3 与 Gen 5 的属性表对这批宝可梦完全一致**（Gen 2 加钢后到 Gen 5 无属性变更）
- 自检程序用 `-DRULE_GEN=n` 与生成器对齐，两套期望值都验证过
- **Mega 形态必须走硬编码白名单 `MEGA_WHITELIST`**（41 个形态 / 39 基础种），不能靠 identifier 含 `-mega`——Veekun 混入了 11 个官方从未实装的占位形态
- Mega 石白名单 `MEGA_STONE_NAMES` 共 39 块，同样不能用 `ite` 后缀匹配
- **烈空坐的 Mega 不需要 Mega 石**，由「龙之波动」直接触发，是唯一特例
- Mega 进化是永久形态（三代规则）。第六代原版是「持续到战斗结束」，本项目按三代处理，实现更简单

---

## 数据源踩坑记录

重新生成数据前务必看一下，这些坑都实际踩过。

### Veekun CSV

1. **`pokemon_moves.csv` 必须完整下载**（10.7 MB / 638322 行 / 1303 pokemon）。网络中断会**静默写出截断文件**（曾只下到 128 KB、只覆盖 pid 1~8），必须校验行数与 unique pid 数
2. `damage_class_id` 是 1/2/3 不是 0/1/2
3. type_id 已是标准表序，按 id 建枚举
4. 多语言表（`move_names` / `type_names` / `ability_names` / `item_names`）只取 `local_language_id == 9`
5. **`pokemon_forms.csv` 的 `is_mega` 字段不可信**——它把官方未实装的占位形态也标成 mega，必须硬编码白名单
6. 道具表无 category 字段，需按英文名推断；道具名不要导出到 Flash（曾占 171 KB）

### 跨世代数据污染（本项目最大的坑）

7. **Veekun 的属性是"最新一代"的，必须回退。** Gen 6 引入 Fairy 属性后改了 18 只一~三代宝可梦 + 3 个招式（撒娇/甜吻/月光）。不处理会导致「钢系打皮皮 = 0x 无效」这种三代规则下不存在的结论，AI 会做出完全错误的判断
8. **Mega 白名单不能靠字符串匹配。** Veekun 的 10000+ 段混着 `clefable-mega` / `starmie-mega` / `raichu-mega-x` / `absol-mega-z` 等 11 个官方从未实装的占位记录（游戏 ROM 里的未启用数据被数据源当成正式形态收录了）
9. **交叉验证信号：Veekun 中有 42 个 `*ite` 道具没有 `item_game_indices` 记录**，这恰好就是官方不存在的 Mega 石（Meteorite 陨石除外，它是正式道具）。可用作白名单的反向校验
10. **独角大盗的 Mega 石英文名是 `Heracronite`，不是 `Heracrossite`**（后者是意大利语/西语译名）。Veekun 和 PokeAPI 都用前者
11. **烈空坐没有 Mega 石**，它的 Mega 由「龙之波动」触发。所以 Veekun 和 PokeAPI 都查不到 `Rayquazite`，这不是数据缺陷
12. **`ite` 后缀匹配会误收** `Meteorite`（陨石 id 537）、`Black Augurite`、`Sablenite` 等正式道具

### 招式效果数据（最大的坑）

14. **`moves.csv` 的 `effect_id` 完全不能用**。它是招式效果**文本条目**的编号，不是效果分类。按它映射出的 `EFFECT_*` 名字全错位：空手切→`EFFECT_TRAPPING_ESCAPE`、连环巴掌→`EFFECT_TOXIC`、剑舞→`EFFECT_MAGNET_RISE`、火焰拳→`EFFECT_EXPLOSION`
15. **正确数据源是 `move_meta.csv`**：`meta_category_id`（招式大类）/ `meta_ailment_id`（异常）/ `ailment_chance` / `stat_chance` / `healing` / `drain` / `crit_rate` / `flinch_chance` / `min_hits` / `max_hits`
16. **能力变化在 `move_meta_stat_changes.csv`**（需单独下载，jsDelivr 偶发失败，用 `raw.githubusercontent.com` 备用）。实测单个招式最多改 **5 项**能力（生长/原始之力/破壳），`MOVE_MAX_STAT_CHANGES` 必须是 5
17. **`ailment_chance == 0` 有两种相反含义**：`MCAT_AILMENT`（电磁波）是**必定触发**，`MCAT_DAMAGE_AILMENT`（火焰拳）才是 0=不触发。不能统一用 `>0` 判断，否则电磁波永远不生效

### 中文名（Veekun 只有繁体）

`*_names.csv` 里中文只有一行，`local_language_id=4`，值是**繁体**
（妙蛙種子）。没有单独的简体行。所以：
- 取 `language_id=4` 再用 `LCMapStringEx` 转简体
- 转换结果要抽查（`test_font.c` 断言 `species[0].name_cn == "妙蛙种子"`）
- 简繁是**多对一**（「裏/裡」都转「里」），所以不能反推

### 特性与道具的效果数据（同样没有机读源）

18. **`ability_effect_text.csv` 和 `item_effects.csv` 都是 404**。`ability_changelog.csv` 只有 id 对照，`item_flavor_text.csv` 只覆盖球类道具（item_id 1~18），`item_fling_effects.csv` 只有 7 种 fling 效果。携带物的数值效果（木炭+20%、粗骨头翻倍）**完全无机读数据，必须手写**
19. **特性表 `kind` 字段曾全是 `ABIL_NONE`**：`emit_abilities` 硬编码的手写枚举清单与 Veekun 的 ability id 对不上，且出现 `ABIL_LEAF_GUARD_2`、`ABIL_SLOW_START_4` 脏后缀。已改为数据驱动生成
20. **道具表 `heal`/`boost_*` 字段曾全是 0**：因为 `item_effects` 加载失败后静默 fallback 到 0。已改为手写 `ITEM_EFFECTS` 表 + 校验测试
21. **`Leftovers`（吃剩的东西）不是树果**，是携带物。按 "berry" 关键词分类会错把它归到 BERRY

### 精灵图处理

0. **`pack_4bpp` 名字叫 4bpp、注释也写 4bpp，代码却是 `& 0x03` << (6-k*2)`
    —— 那是真正的 2bpp**。后果是量化好的 16 色调色板被砍到只剩 4 色
    （实测索引 1 只占 193/4096 像素），精灵图变成无法辨认的碎噪点。
    改成 `& 0x0F` << (4-k*4)` 后恢复正常。
    **为什么没被发现**：C 文件头注释也是错的（写着「2 bit/像素」），
    与代码一致；而渲染代码还没写。**验证精灵图必须把像素导成PNG 肉眼
    看**（`test/dump_sprites.c`），光看代码和数据表看不出画质问题。
    另外「函数名/注释/实现」三处不一致这件事本身就是信号。

13. **LANCZOS 缩放会把 14 色炸成 1000+ 色**并制造大量半透明像素，PNG 压缩完全失效（48×48 缩略图反而比 96×96 原图大 82%）。缩放后**必须立即量化到 16 色**
14. **必须用 PNG-8（P 模式 + transparency）保存**，不能存 RGBA——每像素从 1 字节涨到 4 字节
9. 96×96 缩到 32×32 会丢失全部辨识度（认不出物种）。三代原生图标本身就是 32×32 像素画所以小尺寸可辨；**跨代缩放必须 ≥ 48**
10. 本机网络对 GitHub 不稳定：jsDelivr 单文件可用，打包 API 因仓库超 50 MB 返回 403，高并发下载失败率接近 50%（需压到 3 线程并加指数退避）

### 中文字库（全是「安静地错」，必须导图看）

34. **行字节数用了整数除法**。`CELL_W=14` 不是 8 的倍数，代码里
    `CELL_W // 8` 得 **1**，每行只写 1 字节、**丢掉后 6 个像素**；
    而消费端按 `ceil(14/8)=2` 索引，两边完全错位，字形变成散点——
    「一」本该是一横却渲染出 47 个杂点。修法：统一定义
    `FONT_STRIDE = (CELL_W + 7) // 8`，生成端、消费端、数组长度全用它。
    **教训**：位宽不是 8 的倍数时，整数除法与向上取整的混淆会
    同时破坏数据布局和访问偏移，且两边都不报错。

35. **字号小于字格高度会裁掉笔画**。脚本默认 `--size 12` 而字格改成
    14×14 后没同步，字的上下笔画被画布截断（「妙」只剩 23 个亮点，
    正常 45）。修法：字号默认值取 `CELL_H`。

36. **字符收集只扫了 `gen/`，漏掉 `src/`**。战斗日志的中文
    （"濒死了""第 N 回合""什么都做不了"）全在 `src/poke_battle.c`
    与 `src/poke_ai.c`，只扫生成目录会漏 **26 个字**，
    UI 显示日志时那些字是空白。修法：`gen/` 与 `src/` 都扫。

37. **「优化」反而把字画没了**。为补偿中文字体重心偏下，试过
    「先画 2 倍图画布量 bbox 再平移」，但 bbox 恰好居中时偏移算出来
    是 0，再减半画布就成了负坐标，整张字库全空。实测等线 14px 配
    `anchor="mm"` 本来就是对的位置，**保持最简写法**。

38. **Veekun 没有简体中文行**。`*_names.csv` 的中文只有
    `local_language_id=4`（繁体），简体与繁体共用该行。转换用
    Windows 的 `LCMapStringEx`（`tools/zh_names.py`），
    **不引入 opencc/zhconv**——数据表要能在一台裸 Windows 上重建。

39. **性格表漏了 index 0 占位**。`kNatures` 下标 = nature_id，
    第 0 项是「无性格」占位；把占位行删掉后整表错位一格
    （nature1 变成 lonely 而非 hardy）。`test_font.c` 与
    `data_selfcheck.c` 都加了对应断言。

**共同点**：这一组 bug 全都不影响编译、不影响索引查找、大小也正常，
只有把像素渲染出来才能发现。所以字库和精灵图一样，
改动后必须跑 `make font-preview` 导图肉眼确认。

### 硬编码克制表（手写就会错，必须有回归网）

22. **克制表是 `tools/gen_gen3_data.py` 里手打的 18 个字典，Veekun 无机读源**，所以
    "手算 18×18 = 324 格"是不现实的，实际踩了 5 处错：
    - `Flying` 误写成打草/毒 2x（实际都是 1x）→ 飞行打 Venusaur 算出 4 倍克制，
      AI 于是放着 2 倍的火焰不用去用飞行
    - `Fire` 误写成打钢 2x（实际 0.5x，钢抗火）
    - `Grass` 误写成打虫 0.5x（实际 2x）
    - `Psychic` 漏了打妖精 2x、`Fighting` 漏了打妖精 0.5x（Gen VI 新增属性）
    修法：`tools/regen_types.py` 独立重建这张表 + 内置 16 条关键组合回归，
    并与 `gen_gen3_data.py` 的副本做同步校验（不一致直接报错）。
    `test_calc.c` 里另有一组 22 条断言钉住双属性组合。
23. **`MOVE_COUNT` 是招式 id 上界不是数组长度**。数组按 id 1..max_id 生成，
    `kMoves[0]` 是隐式零初始化占位，有效下标 1..353。写成
    `for (i = 1; i <= MOVE_COUNT; i++)` 会越界读一格（gcc 的
    `-Waggressive-loop-optimizations` 能报出来）。正确写法是 `i < MOVE_COUNT`

### 对战流程的空转陷阱（会导致「AI 很笨」的假象）

24. **`battle_step` 的返回值必须逐个处理**：`1` = 我方需换人、`2` = 对方需换人、
    `3` = 双方都需换人。漏掉任何一个（比如只写 `if (rc == 1)`），
    濒死的那一方永远换不了人，双方出战位都判 0 血而整回合静默空转，
    战斗会一直卡到回合上限。实测这会让「200 局强度测试」单局跑超 100 秒。
25. **调用方自己的决策器也必须处理濒死**。测试里的「随机手」最初在
    出战位 0 血时仍然照常出招（因为招式槽非空就 return），
    结果永远不换人 —— 这是测试代码的 bug，不是引擎的 bug，
    但两者表现完全一样（卡在同一步），排查时要先确认是哪一侧。
26. **AI 的蒙特卡洛模拟里也要判「无药可救」**。`mc_playout` 若在某方濒死
    且替补全灭时只 `break` 出内层循环，外层 `while` 会一直转到 node_cap。
    现在直接 `break` 出整个 playout（这一局已经分出胜负）。
27. **不要用 `guard++ < 100000` 兜底**。真实的 bug 会被 10 万次空转掩盖成
    「测试超时」，看不出问题在哪。改成 600 步 + `battle_step` 返回 -1 但
    `result` 仍为 ONGOING 时立即 break，让断言暴露出来。

### 索引约定错位（最隐蔽的一类 bug）

29. **`kMoves[]` 的下标是 `move_id - 1`**，不是 `move_id`。
    引擎曾写 `kMoves[atk->move[slot]]`，于是**每个招式都变成隔壁那一招**
    （喷火龙拿到 Mist：威力 0、PP 30），实测 280/353 个 id 装配错误；
    `move_id == MOVE_COUNT` 时还会越界读 `.rodata` 后面的垃圾。
    它不崩溃、不越界（除了最后一格），只是数据 quietly 错了。
    **为什么长期没被发现**：测试侧也用 `&kMoves[i]`（i 当 id），两边
    错位正好互相抵消，所有断言都绿。这类 bug 必须从「装配→读回」
    端到端测试才抓得到（`test_battle.c` 的 `[索引]` 段就是为此加的）。
    修法：`gen/poke_moves.h` 里加 `mv_get(id)` 访问器，把校验和
    偏移规则收敛到一处，禁止裸下标。
30. **`mon_set_iv` / `statsheet_set_ev` 的 `stat_idx` 是 1-based**，
    但边界检查只挡了 `> 6`。传 0 会让 `iv[-1]` 写到相邻字段
    （实测正好落在 `base.mega`，静默破坏 Mega 还原）。

### 「搜索/新特性没生效」的两种原因

31. **选候选的循环起点写错，搜索结果被丢弃**。曾经是
    `for (i = 0; ...) { if (legal) { chosen = i; break; } }`，
    而 `cands` 已按 L1 分降序——于是无论L2/L3 算出什么，最终都返回
    `cands[0]`。三个档位输出逐字节相同，而所有「决策质量」测试照样全绿。
    **这类 bug 需要差分测试才能发现**：把搜索配置关掉（`my_cands`/
    `opp_replies`/`mc_playouts` 全 0）与正常档位对比，断言决策有差异。
    现在 `test_ai.c` 的 `[搜索]` 段就是这个用途。
32. **搜索分与启发式分量纲不一致会淹没启发式**。L1 是 1e4 量级的战术分
    （能打死人/ 会被秒 / 该不该强化），搜索分是 `ai_eval_position` 的
    1e5~1e6 物资项。直接用搜索分替代 L1，只看 HP 和种族值的 eval 会
    主导决策，实测胜率 73% → 47%。**搜索应该是修正项**：归一化到
    [-1,1] 再乘小权重叠加。
33. **min-over-replies 让 AI 变笨**。取「对手让我最难堪的应手」是防最坏
    情况，不是最大化收益；配合 ±1e6 的胜负分，会让 AI 永远挑不会被秒
    的保守招。改成按对手威胁加权的期望后，胜率从 48% 回到 70%。

### 衡量 AI 强弱必须带基线

28. **「AI 对随机手胜率 >= 75%」这种绝对阈值没有意义**。两队都是随机生成，
    队伍强度本身方差很大，16 局样本里 side0 随机胜率能在 43%~63% 之间浮动。
    实测 16 局时 AI 只赢 11 局（69%）看起来很弱，但扩到 200 局后：
    **AI 70% vs 随机基线 52%**，优势稳定。所以强度测试要：
    - 跑同批种子的「随机 vs 随机」对照组；
    - 断言 **AI 胜率 − 基线胜率 >= 10 个百分点**，而非绝对值；
    - 默认样本量 200 局（约 16 秒，300 局实测 25.6 秒）。

---

## 路线图

### 已完成

数据层（六张表 + 中文名）、规则层（能力值 / 伤害 / 6v6 回合流程 / 特性 / 道具 / Mega）、
对战 AI（三层）、精灵图、两套中文字库。

### 待做（按依赖排序）

| # | 事项 | 说明 |
|---|---|---|
| 1 | **渲染层最小闭环** | `poke_render` + `dump_screen`：先做到「能把画面导成图看」。精灵图和字库的教训是渲染问题只能靠看图发现 |
| 2 | **按键 + 最小状态机** | 用 `test_ui` 喂按键序列，不上硬件就能验证光标与状态迁移 |
| 3 | **战斗界面** | 含日志逐条播放、强制换人分支、招式菜单 |
| 4 | **队伍配置**（或挪到小程序） | 见[界面设计](#界面设计待实现)。先用手写死的队伍跑通全流程更划算 |
| 5 | **NVS 存档 + 结算画面** | 烧录注意：从 `0x0` 烧合并 `full.bin` **会重置 NVS**，要保数据得用分段 `idf.py flash` |
| 6 | **接入官方 BSP + LVGL** | 替换掉自制渲染方案，改用 `components/bsp` |

### 已知待定

- **AI 难度档位数量不一致**：需求写 5 档（简单 / 普通 / 困难 / 大师 / 大师+），
  `src/poke_ai.h` 目前只有 3 档（`ROOKIE` / `SMART` / `EXPERT`）。
  UI 的难度选择要么按 3 档做，要么先补齐另外 2 档。
- **字体在 16px 下偏细**：得意黑是倾斜的**标题体**、只有一个字重，16px 下笔画约 1px，
  4bpp 抗锯齿后字形发虚。同一批字渲染成 1bpp 点阵反而更清晰（见
  [字体对比图](#中文字库)）。若最终走 LVGL 路线，建议再生成一份常规直立字体
  （如 Noto Sans SC）对比。
- **字对血条框偏大、1bpp 字库缺半角 ASCII**：见[界面设计](#界面设计待实现)里
  「两个字体相关的真实缺口」。这两条会直接影响战斗界面的实现方式，建议早定。

---

## 许可

### 本项目代码：MIT

`src/`、`test/`、`tools/`、`Makefile` 等本项目自己写的代码采用 **MIT**，
全文见 [LICENSE](LICENSE)。

> **MIT 只覆盖代码本身。** 仓库里还包含第三方内容的衍生物（精灵图、字库位图），
> 它们**不受 MIT 覆盖**，另有各自的条款，见下节。分发本项目时请把这一点一并说明。

### 第三方资源

本项目**不包含**任何原版游戏素材，数据与字体均来自公开数据集。重建方法见
[docs/data-sources.md](docs/data-sources.md)。

| 资源 | 用途 | 许可 |
|---|---|---|
| [Veekun 数据集](https://github.com/veekun/pokedex) | 三代种族 / 招式 / 特性 / 道具 / 性格（`data/csv/`，不入库） | CC-BY-SA 3.0 |
| [PokeAPI/sprites](https://github.com/PokeAPI/sprites) | 精灵图（`gen/poke_sprites.c` 的来源） | 游戏素材，版权归任天堂 / Creatures / GAME FREAK |
| [得意黑 Smiley Sans](https://github.com/atelier-anchor/smiley-sans) | LVGL 4bpp 字库（`gen/font_app_16.c` 的来源） | SIL OFL 1.1，全文见 [docs/third-party/SmileySans-OFL.txt](docs/third-party/SmileySans-OFL.txt) |
| 等线（Windows 系统字体） | 自制 1bpp 点阵字库（`gen/font_cn.c`） | 随 Windows 分发，**仅供本机自用**；要公开发布请换成 OFL 字体重新生成 |

宝可梦（Pokémon）是任天堂 / Creatures / GAME FREAK 的注册商标，相关名称、形象与
招式设计的权利归其各自所有者。本项目为自用学习项目，不作商业用途。

---

## 相关链接

- **设备与官方仓**
  - AI Passport 官网 <https://ai-passport.folotoy.cn/>
  - FoloToy/ai-passport —— BSP、LVGL 中文字体文档、BLE 伴侣小程序参考
    <https://github.com/FoloToy/ai-passport>
- **数据源**
  - Veekun 数据集 <https://github.com/veekun/pokedex>
  - PokeAPI/sprites <https://github.com/PokeAPI/sprites>
- **机制与算法参考**
  - 对战机制 pret/pokered <https://github.com/pret/pokered>
  - AI 评估函数权重 Technical Machine —— 只借鉴算法不借鉴代码（它是 TypeScript，
    且依赖 PS 引擎做前向模拟，无法移植到 8 MB Flash）
