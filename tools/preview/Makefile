.DEFAULT_GOAL := build/preview
# Хост-превью UI: настоящий код режимов/LVGL, железо заменено заглушками (host_hw.cpp).
# make -C tools/preview && ./build/preview out_dir   (PNG экранов; потом: python3 tools/preview/topng.py out_dir)
ROOT := ../..
SRC := $(ROOT)/src
B := build
CXXFLAGS := -std=gnu++17 -O1 -g -fpermissive -w -I stub -I $(SRC) -I $(SRC)/external/lvgl -I $(SRC)/external/printf \
  -DLV_CONF_INCLUDE_SIMPLE -D_sbrk=host_unused_sbrk -DHOST_PREVIEW
CFLAGS := -std=gnu11 -O1 -g -w -I stub -I $(SRC) -I $(SRC)/external/lvgl -DLV_CONF_INCLUDE_SIMPLE -DHOST_PREVIEW

APP := $(SRC)/radio.cpp $(SRC)/bands.cpp $(SRC)/modes/mode.cpp $(SRC)/modes/vfo.cpp $(SRC)/modes/radio.cpp $(SRC)/modes/scanner.cpp $(SRC)/modes/bc_scanner.cpp $(SRC)/modes/analyser.cpp \
  $(SRC)/modes/fc.cpp $(SRC)/modes/loot_screen.cpp $(SRC)/modes/channels.cpp $(SRC)/ui/menu_overlay.cpp $(SRC)/ui/statusline.cpp $(SRC)/ui/graphics.cpp \
  $(SRC)/ui/numval.cpp $(SRC)/ui/spectrum.cpp $(SRC)/ui/lv_port.cpp $(SRC)/helper/lootlist.cpp \
  $(SRC)/helper/measurements.cpp $(SRC)/helper/log.cpp $(SRC)/ui/lv_helpers.cpp \
  $(SRC)/helper/names.cpp $(SRC)/helper/channels.cpp
LVGL := $(shell find $(SRC)/external/lvgl/src -name '*.c' ! -path '*/extra/*' ! -path '*/draw/nxp/*' \
  ! -path '*/draw/sdl/*' ! -path '*/draw/arm2d/*' ! -path '*/draw/renesas/*' ! -path '*/draw/stm32_dma2d/*' \
  ! -path '*/draw/swm341_dma2d/*' ! -name 'lv_font_montserrat_*' ! -name 'lv_font_dejavu_*' \
  ! -name 'lv_font_simsun_*' ! -name 'lv_font_unscii_*')
FONTS := $(wildcard $(SRC)/ui/lvgl_fonts/*.c)
HOST := host_hw.cpp preview.cpp

OBJS := $(patsubst $(ROOT)/%.cpp,$(B)/%.o,$(APP)) $(patsubst %.cpp,$(B)/%.o,$(HOST)) \
  $(patsubst $(ROOT)/%.c,$(B)/%.o,$(LVGL) $(FONTS))

# lv_conf.h для хоста: тот же, но куча LVGL больше (64-битные указатели)
stub/lv_conf.h: $(SRC)/lv_conf.h
	sed 's/#define LV_MEM_SIZE (12 \* 1024)/#define LV_MEM_SIZE (512 * 1024)/' $< > $@

$(B)/preview: stub/lv_conf.h $(OBJS)
	g++ -o $@ $^ -lm

$(B)/%.o: $(ROOT)/%.cpp
	@mkdir -p $(dir $@)
	g++ $(CXXFLAGS) -c $< -o $@
$(B)/%.o: $(ROOT)/%.c
	@mkdir -p $(dir $@)
	gcc $(CFLAGS) -c $< -o $@
$(B)/%.o: %.cpp
	@mkdir -p $(dir $@)
	g++ $(CXXFLAGS) -c $< -o $@
$(B)/%.o: %.c
	@mkdir -p $(dir $@)
	gcc $(CFLAGS) -c $< -o $@
