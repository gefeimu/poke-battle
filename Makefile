# poke-battle 构建与测试
#
# 四组测试各有不同的编译开关，历史上就是因为散在命令行上容易漏
# （calc_rand_85_100 未覆盖导致链接失败、-DPOKE_BATTLE_TEST_NO_RNG
# 漏加导致 battle_rng 重复定义），所以统一收在这里。
#
#   make            编译全部
#   make test       编译并跑全部测试（含跨文件一致性检查）
#   make bench      AI 决策耗时剖析
#   make types      重新生成属性克制表（改克制关系后必须跑）
#   make sprites         重新生成精灵图（需指定 SPRITES_SRC 源图目录）
#   make sprites-preview 把固件里的精灵图导成 PNG 肉眼检查画质
#   make font            重新生成自制 1bpp 字库（需指定 FONT 字体路径）
#   make lvgl-font       重新生成 LVGL 4bpp 字库（需指定 FONT 字体路径）
#   make font-preview    把自制字库渲染成图片，肉眼检查字形
#   make readme-images   重出 README 配图（docs/img/）
#   make screen-preview  把固件渲染层画的战斗界面导成 PNG 肉眼验收
#   make render-compare  比对「固件渲染」与「PC 预览」的同一屏
#   make clean      清理

# 注意：用 override 而不是 ?=。本机环境的 CC 变量被设成了 "cc"
#（在 Git Bash 下不存在该命令），?= 不会覆盖已有值。
override CC := gcc
override CFLAGS := -O2 -Wall -Wextra
INC      = -Igen -Isrc
# 单测覆盖引擎内的抽签实现，让所有断言可重放
TESTFLAGS = -DPOKE_BATTLE_TEST_NO_RNG -DPOKE_CALC_TEST_NO_RAND

# data_selfcheck 只校验数据表本身，不需要战斗引擎，也就不需要
# calc_rand_85_100（它不覆盖这个符号）。
SRC_DATA = gen/poke_moves.c gen/poke_species.c gen/poke_types.c \
           gen/poke_abilities.c gen/poke_items.c gen/poke_natures.c \
           gen/poke_learnsets.c

SRC = src/poke_battle.c src/poke_ability.c src/poke_item.c src/poke_calc.c \
      gen/poke_abilities.c gen/poke_items.c gen/poke_moves.c \
      gen/poke_species.c gen/poke_types.c gen/poke_natures.c \
      gen/poke_learnsets.c gen/poke_sprites.c

# AI 相关的测试/基准额外链接 AI 模块
SRC_AI = $(SRC) src/poke_ai.c

# 渲染层：**不依赖战斗引擎**，只吃精灵图数据与字体数据。
# 所以渲染相关的测试/工具都能单独编译、单独跑。
RENDER_SRC = src/poke_render.c src/poke_font.c src/poke_ui.c \
             gen/poke_font_data.c gen/poke_sprites.c gen/poke_species.c

# 输入层与界面状态机：既不吃引擎，也不吃渲染层 —— 上游只有按键事件。
# 这两个是唯一「跑一遍不到一毫秒」的测试，改完随手就能重跑。
UI_SRC    = src/poke_input.c src/poke_ui_state.c

# 会话层（把状态机接到引擎）：唯一一个「整机链路」的测试 —— 引擎、AI、
# 状态机、字库（日志折行要量像素宽）全都要。所以它链接得最重，但换来的
# 是「喂一串按键就能打完一整场 6v6 并断言每一处状态迁移」。
# 注意这里**没有**渲染层：会话只吐 BattleScreen / MenuScreen（纯数据），
# 不画像素。
SESSION_SRC = src/poke_session.c src/poke_ui_state.c src/poke_input.c \
              src/poke_font.c

# 屏幕导出工具要的是**整条链路**：引擎 + AI + 会话 + 渲染层。
# **不能直接拼 $(SRC_AI) $(SESSION_SRC) $(RENDER_SRC)** —— 三份名单里有
# 重复项（poke_font.c / poke_sprites.c / poke_species.c），同一份源码在一
# 条编译命令里出现两次会重复定义符号，链接直接失败。所以这里显式列全，
# 每份只出现一次。
SCREEN_SRC = src/poke_battle.c src/poke_ability.c src/poke_item.c \
             src/poke_calc.c src/poke_ai.c src/poke_session.c \
             src/poke_ui_state.c src/poke_input.c src/poke_render.c \
             src/poke_font.c src/poke_ui.c \
             gen/poke_abilities.c gen/poke_items.c gen/poke_moves.c \
             gen/poke_species.c gen/poke_types.c gen/poke_natures.c \
             gen/poke_learnsets.c gen/poke_sprites.c gen/poke_font_data.c

BUILD = build
TESTS = data_selfcheck test_calc test_battle test_ai test_sprites test_font \
        test_render test_input test_ui test_session

.PHONY: all test check-consistency bench types sprites sprites-preview \
        font lvgl-font lvgl-font-to-c font-preview readme-images \
        screen-preview render-compare clean

all: $(BUILD)

$(BUILD):
	@mkdir -p $(BUILD)

$(BUILD)/data_selfcheck: test/data_selfcheck.c $(SRC_DATA) | $(BUILD)
	$(CC) $(CFLAGS) $(INC) -o $@ $< $(SRC_DATA)

$(BUILD)/test_calc: test/test_calc.c $(SRC) | $(BUILD)
	$(CC) $(CFLAGS) $(INC) $(TESTFLAGS) -o $@ $< $(SRC)

$(BUILD)/test_battle: test/test_battle.c $(SRC) | $(BUILD)
	$(CC) $(CFLAGS) $(INC) $(TESTFLAGS) -o $@ $< $(SRC)

$(BUILD)/test_ai: test/test_ai.c $(SRC_AI) | $(BUILD)
	$(CC) $(CFLAGS) $(INC) $(TESTFLAGS) -o $@ $< $(SRC_AI)

# 精灵图自检只依赖图片数据，不链接战斗引擎
$(BUILD)/test_sprites: test/test_sprites.c gen/poke_sprites.c gen/poke_species.c | $(BUILD)
	$(CC) $(CFLAGS) $(INC) -o $@ $< gen/poke_sprites.c gen/poke_species.c

# 字库自检要读六张中文名表，所以链接全部数据表（不链接引擎）
FONT_DATA = gen/font_cn.c gen/poke_species.c gen/poke_moves.c gen/poke_types.c \
            gen/poke_abilities.c gen/poke_items.c gen/poke_natures.c

$(BUILD)/test_font: test/test_font.c $(FONT_DATA) | $(BUILD)
	$(CC) $(CFLAGS) $(INC) -o $@ $< $(FONT_DATA)

# 渲染层自检。断言分两类：可判定的（越界/返回值/查表边界）直接断言，
# 「只能看图发现」的（配色/版式）用取点验证。
$(BUILD)/test_render: test/test_render.c $(RENDER_SRC) | $(BUILD)
	$(CC) $(CFLAGS) $(INC) -o $@ $< $(RENDER_SRC)

# 三键驱动自检：喂「电压序列 + 毫秒时基」，断言窗口边界/去抖/长按。
#   注意这里**故意不带 TESTFLAGS**：输入层没有任何抽签，
#   带上反而会引入没用的开关。
$(BUILD)/test_input: test/test_input.c src/poke_input.c | $(BUILD)
	$(CC) $(CFLAGS) $(INC) -o $@ $< src/poke_input.c

# 界面状态机自检：喂按键序列，断言界面/光标/滚动窗口/吐出的动作。
$(BUILD)/test_ui: test/test_ui.c $(UI_SRC) | $(BUILD)
	$(CC) $(CFLAGS) $(INC) -o $@ $< $(UI_SRC)

# 会话层自检：喂按键序列跑完整场对局，断言引擎状态、日志播放游标、
# 强制换人分支与结算。这是唯一需要「固定随机源」的界面侧测试 ——
# 对手是 AI，不钉住抽签就没有可重放的结果。
# 额外要 gen/poke_font_data.c：日志折行是量真实像素宽的，不是估字数。
$(BUILD)/test_session: test/test_session.c $(SRC_AI) $(SESSION_SRC) \
                       gen/poke_font_data.c | $(BUILD)
	$(CC) $(CFLAGS) $(INC) $(TESTFLAGS) -o $@ $< $(SRC_AI) $(SESSION_SRC) \
	      gen/poke_font_data.c

$(BUILD)/bench_ai: test/bench_ai.c $(SRC_AI) | $(BUILD)
	$(CC) $(CFLAGS) $(INC) $(TESTFLAGS) -o $@ $< $(SRC_AI)

# 顺序有意义：先生成/编译全部测试，再跑一致性检查。
# 检查里的「README 测试计数」是**实测**的（跑一遍二进制读它的汇总行），
# 二进制还不存在就没得比。串行 make 会按左到右的顺序构建前置目标。
test: $(addprefix $(BUILD)/,$(TESTS)) check-consistency
	@for t in $(TESTS); do \
	  echo "======== $$t ========"; \
	  $(BUILD)/$$t || exit 1; \
	  echo ""; \
	done

# 精灵图/文档的交叉一致性检查。精灵图的 bug 曾经「文档也跟着错」，
# 没有任何一处能自查出来，所以把跨文件约束写成断言。
check-consistency:
	@python tools/check_consistency.py

bench: $(BUILD)/bench_ai
	$(BUILD)/bench_ai

types:
	python tools/regen_types.py

# 把固件里的精灵图导成 PNG 肉眼检查画质。
# 精灵图的 bug（位深错、色表截断）光看代码发现不了，必须看图。
$(BUILD)/dump_sprites: test/dump_sprites.c gen/poke_sprites.c gen/poke_species.c | $(BUILD)
	$(CC) $(CFLAGS) $(INC) -o $@ $< gen/poke_sprites.c gen/poke_species.c

sprites-preview: $(BUILD)/dump_sprites
	@mkdir -p data/sprites_preview
	$(BUILD)/dump_sprites

# 把固件里的字库渲染成 PGM 图片，肉眼检查字形。
# 用法同 sprites-preview，也可 `make font-preview TEXT="要渲染的文字"`。
$(BUILD)/dump_font: test/dump_font.c gen/font_cn.c | $(BUILD)
	$(CC) $(CFLAGS) $(INC) -o $@ $< gen/font_cn.c

font-preview: $(BUILD)/dump_font
	@mkdir -p data/font_preview
	@if [ -n "$(TEXT)" ]; then $(BUILD)/dump_font $(TEXT); else $(BUILD)/dump_font; fi

# 重新生成字库（两套方案，各自独立）。
#   make font      FONT=C:/Windows/Fonts/Deng.ttf            自制 1bpp 点阵
#   make lvgl-font FONT=data/fonts/SmileySans-Oblique.ttf    LVGL 4bpp 抗锯齿
font:
	@test -n "$(FONT)" || \
	  (echo "用法: make font FONT=<中文字体路径.ttf>"; exit 1)
	python tools/gen_font.py --font $(FONT)

# 一次出三套字号：16px 给对话框，12px 给血条框的名字与数字，
# 10px 只含 ASCII 段、专给血条框里的 HP 标签（位图仅 1.7 KB）
lvgl-font:
	@test -n "$(FONT)" || \
	  (echo "用法: make lvgl-font FONT=<中文字体路径.ttf>"; exit 1)
	python tools/gen_lvgl_font.py --font $(FONT) --chars gen/font_chars.txt \
	  --out gen/font_app_16.c --name font_app_16 --size 16
	python tools/gen_lvgl_font.py --font $(FONT) --chars gen/font_chars.txt \
	  --out gen/font_app_12.c --name font_app_12 --size 12
	python tools/gen_lvgl_font.py --font $(FONT) --symbols HP --no-subset \
	  --out gen/font_hp_10.c --name font_hp_10 --size 10
	@$(MAKE) --no-print-directory lvgl-font-to-c

# 把 lv_font_conv 的输出转成项目自有格式（gen/poke_font_data.c）。
# src/ 不能依赖 lvgl.h，必须走这一步；理由见 src/poke_font.h 的注释。
lvgl-font-to-c:
	python tools/lvgl_font_to_c.py --out gen/poke_font_data.c \
	  gen/font_app_16.c gen/font_app_12.c gen/font_hp_10.c

# 重出 README 配图。图里的精灵图与文字都从固件数据解出，所以数据一改
# 就要重跑，不要手画。生成后顺带做一次字库覆盖校验。
readme-images:
	python tools/make_readme_images.py

# 把固件渲染层画的**七屏**界面导成图片。
# 渲染的 bug 不崩溃、不报错，只是画出来不对 —— 只能看图发现。
# 七屏各自的版式不同源（战斗界面照原版，菜单是自己定的），所以一屏一张图，
# 混在一起看容易漏掉某一屏没画。
$(BUILD)/dump_screen: test/dump_screen.c $(SCREEN_SRC) | $(BUILD)
	$(CC) $(CFLAGS) $(INC) $(TESTFLAGS) -o $@ $< $(SCREEN_SRC)

SCRIPTS_SCREENS = main team log battle action confirm switch result

screen-preview: $(BUILD)/dump_screen
	@mkdir -p data/screen_preview
	$(BUILD)/dump_screen --all
	@for n in $(SCRIPTS_SCREENS); do \
	  python tools/ppm2png.py data/screen_preview/$$n.ppm \
	    data/screen_preview/$$n.png --scale 3 || exit 1; \
	done

# 比对「固件渲染层」与「PC 预览」画的同一屏。
# 两边是独立实现，允许 RGB565 量化与边缘 1px 的差异；
# 差异 >32 的像素低于 2% 算通过（阈值与理由见 tools/compare_screen.py）。
render-compare: screen-preview readme-images
	python tools/compare_screen.py --c data/screen_preview/battle.ppm \
	  --py docs/img/hero.png --scale 3 \
	  --heatmap data/screen_preview/diff.png

# SPRITES_SRC 在命令行给，例如：
#   make sprites SPRITES_SRC=/d/Download/Chromedownload/sprites/sprites/pokemon/versions
sprites:
	@test -n "$(SPRITES_SRC)" || \
	  (echo "用法: make sprites SPRITES_SRC=<sprites仓库>/sprites/pokemon/versions"; exit 1)
	python tools/build_sprites_c.py --src $(SPRITES_SRC) --gen3 firered-leafgreen

clean:
	@rm -rf $(BUILD)
	@echo "已清理"