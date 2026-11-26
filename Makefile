TARGET  := rt880-fw-by-fagci
BUILD   := build

PREFIX  := arm-none-eabi
CXX     := $(PREFIX)-g++
CC      := $(PREFIX)-gcc
OBJCOPY := $(PREFIX)-objcopy
SIZE    := $(PREFIX)-size

MCUFLAGS := -mcpu=cortex-m4 -mthumb -mfloat-abi=soft

C_DEFS := \
  -DAT32F423RCT7 \
  -DUSE_STDPERIPH_DRIVER \
  -DLV_CONF_INCLUDE_SIMPLE

C_INCLUDES := \
  -Isrc \
  -Isrc/external/lvgl \
  -Iinc \
  -Isrc/external/cmsis/cm4/core_support \
  -Isrc/external/cmsis/cm4/device_support \
  -Isrc/external/drivers/inc \
  -Isrc/external/printf

CXXFLAGS := $(MCUFLAGS) -ffunction-sections -fdata-sections -fno-exceptions -fno-rtti -Wall -Wextra -Os -g3 \
           -ffast-math \
           -Wno-enum-int-mismatch -Wno-aggressive-loop-optimizations \
           -Wno-unused-function -Wno-unused-but-set-variable \
           $(C_DEFS) $(C_INCLUDES)
ASFLAGS := $(MCUFLAGS) -x assembler-with-cpp -g3 $(C_DEFS) $(C_INCLUDES)
LDFLAGS := $(MCUFLAGS) -Tlinker.ld -Wl,--gc-sections -Wl,-Map=$(BUILD)/$(TARGET).map --specs=nano.specs
LIBS    := -lc -lstdc++

SRCS_CXX := $(shell find src -name '*.cpp' ! -path '*/external/*' | sort)
SRCS_LVGL := $(shell find src/external/lvgl/src -name '*.c' ! -path '*/extra/*' \
  ! -path '*/draw/nxp/*' ! -path '*/draw/sdl/*' ! -path '*/draw/arm2d/*' \
  ! -path '*/draw/renesas/*' ! -path '*/draw/stm32_dma2d/*' \
  ! -path '*/draw/swm341_dma2d/*' ! -name 'lv_font_montserrat_*' \
  ! -name 'lv_font_dejavu_*' ! -name 'lv_font_simsun_*' ! -name 'lv_font_unscii_*' | sort)

SRCS_C := $(shell find src -name '*.c' ! -path '*/external/*' \
  -o -name '*.c' -path '*/external/drivers/src/at32f423_adc.c' \
  -o -name '*.c' -path '*/external/drivers/src/at32f423_crm.c' \
  -o -name '*.c' -path '*/external/drivers/src/at32f423_dma.c' \
  -o -name '*.c' -path '*/external/drivers/src/at32f423_flash.c' \
  -o -name '*.c' -path '*/external/drivers/src/at32f423_gpio.c' \
  -o -name '*.c' -path '*/external/drivers/src/at32f423_i2c.c' \
  -o -name '*.c' -path '*/external/drivers/src/at32f423_misc.c' \
  -o -name '*.c' -path '*/external/drivers/src/at32f423_spi.c' \
  -o -name '*.c' -path '*/external/drivers/src/at32f423_usart.c' \
  -o -name '*.c' -path '*/external/cmsis/cm4/device_support/system_at32f423.c' \
  -o -name '*.c' -path '*/external/printf/printf.c' \
  | sort)

# Ищем оба варианта расширения: *.s и *.S
SRCS_S := $(shell find src \( -name '*.s' -o -name '*.S' \) ! -path '*/external/*' | sort)

OBJS := $(patsubst %.cpp,$(BUILD)/%.o,$(SRCS_CXX)) \
        $(patsubst %.c,$(BUILD)/%.o,$(SRCS_C)) \
        $(patsubst %.c,$(BUILD)/%.o,$(SRCS_LVGL)) \
        $(patsubst %.S,$(BUILD)/%.o,$(filter %.S,$(SRCS_S))) \
        $(patsubst %.s,$(BUILD)/%.o,$(filter %.s,$(SRCS_S)))

all: $(BUILD)/$(TARGET).elf $(BUILD)/$(TARGET).bin $(BUILD)/$(TARGET).hex

$(BUILD)/$(TARGET).elf: $(OBJS)
	@mkdir -p $(dir $@)
	$(CXX) $(OBJS) $(LDFLAGS) $(LIBS) -o $@
	$(SIZE) $@

$(BUILD)/$(TARGET).bin: $(BUILD)/$(TARGET).elf
	$(OBJCOPY) -O binary $< $@

$(BUILD)/$(TARGET).hex: $(BUILD)/$(TARGET).elf
	$(OBJCOPY) -O ihex $< $@

# stage-2 (fast-прошивка) и общий образ для стокового загрузчика.
boot2:
	$(MAKE) -C boot2

install-image: all boot2
	python3 tools/mkimage.py $(BUILD)/boot2.bin $(BUILD)/$(TARGET).bin $(BUILD)/rt880-install.bin

$(BUILD)/%.o: %.cpp
	@mkdir -p $(dir $@)
	$(CXX) -c -MMD -MP $(CXXFLAGS) $< -o $@

$(BUILD)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) -c -MMD -MP $(CXXFLAGS) $< -o $@

# Оба правила нужны: GNU make не делает case-insensitive match
$(BUILD)/%.o: %.S
	@mkdir -p $(dir $@)
	$(CC) -c $(ASFLAGS) $< -o $@

$(BUILD)/%.o: %.s
	@mkdir -p $(dir $@)
	$(CC) -c $(ASFLAGS) $< -o $@

# зависимости от заголовков (lv_conf.h и др.): правка заголовка пересобирает зависимые
-include $(OBJS:.o=.d)

clean:
	rm -rf $(BUILD)

.PHONY: all clean boot2 install-image