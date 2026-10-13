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

BUILD = build
TESTS = data_selfcheck test_calc test_battle test_ai test_sprites test_font

.PHONY: all test check-consistency bench types sprites sprites-preview \
        font lvgl-font font-preview readme-images clean

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

$(BUILD)/bench_ai: test/bench_ai.c $(SRC_AI) | $(BUILD)
	$(CC) $(CFLAGS) $(INC) $(TESTFLAGS) -o $@ $< $(SRC_AI)

test: check-consistency $(addprefix $(BUILD)/,$(TESTS))
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

# 一次出两套字号：16px 给对话框，12px 给血条框（16px 在 128x32 的框里偏大）
lvgl-font:
	@test -n "$(FONT)" || \
	  (echo "用法: make lvgl-font FONT=<中文字体路径.ttf>"; exit 1)
	python tools/gen_lvgl_font.py --font $(FONT) --chars gen/font_chars.txt \
	  --out gen/font_app_16.c --name font_app_16 --size 16
	python tools/gen_lvgl_font.py --font $(FONT) --chars gen/font_chars.txt \
	  --out gen/font_app_12.c --name font_app_12 --size 12

# 重出 README 配图。图里的精灵图与文字都从固件数据解出，所以数据一改
# 就要重跑，不要手画。生成后顺带做一次字库覆盖校验。
readme-images:
	python tools/make_readme_images.py

# SPRITES_SRC 在命令行给，例如：
#   make sprites SPRITES_SRC=/d/Download/Chromedownload/sprites/sprites/pokemon/versions
sprites:
	@test -n "$(SPRITES_SRC)" || \
	  (echo "用法: make sprites SPRITES_SRC=<sprites仓库>/sprites/pokemon/versions"; exit 1)
	python tools/build_sprites_c.py --src $(SPRITES_SRC) --gen3 firered-leafgreen

clean:
	@rm -rf $(BUILD)
	@echo "已清理"