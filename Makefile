TARGET  := rt880-fw-by-fagci
BUILD   := build

PREFIX  := arm-none-eabi
CC      := $(PREFIX)-gcc
OBJCOPY := $(PREFIX)-objcopy
SIZE    := $(PREFIX)-size

MCUFLAGS := -mcpu=cortex-m4 -mthumb -mfloat-abi=soft

C_DEFS := \
  -DAT32F423RCT7 \
  -DUSE_STDPERIPH_DRIVER

C_INCLUDES := \
  -Isrc \
  -Iinc \
  -Isrc/external/cmsis/cm4/core_support \
  -Isrc/external/cmsis/cm4/device_support \
  -Isrc/external/drivers/inc \
  -Isrc/external/printf

CFLAGS  := $(MCUFLAGS) -ffunction-sections -fdata-sections -Wall -Wextra -Os -g3 \
           -ffast-math \
           -Wno-enum-int-mismatch -Wno-aggressive-loop-optimizations \
           -Wno-unused-function -Wno-unused-but-set-variable \
           $(C_DEFS) $(C_INCLUDES)
ASFLAGS := $(MCUFLAGS) -x assembler-with-cpp -g3 $(C_DEFS) $(C_INCLUDES)
LDFLAGS := $(MCUFLAGS) -Tlinker.ld -Wl,--gc-sections -Wl,-Map=$(BUILD)/$(TARGET).map --specs=nano.specs
LIBS    := -lc

SRCS_C := $(shell find src -name '*.c' ! -path '*/external/*' \
  -o -name '*.c' -path '*/external/drivers/src/at32f423_adc.c' \
  -o -name '*.c' -path '*/external/drivers/src/at32f423_crm.c' \
  -o -name '*.c' -path '*/external/drivers/src/at32f423_dma.c' \
  -o -name '*.c' -path '*/external/drivers/src/at32f423_flash.c' \
  -o -name '*.c' -path '*/external/drivers/src/at32f423_gpio.c' \
  -o -name '*.c' -path '*/external/drivers/src/at32f423_misc.c' \
  -o -name '*.c' -path '*/external/drivers/src/at32f423_spi.c' \
  -o -name '*.c' -path '*/external/cmsis/cm4/device_support/system_at32f423.c' \
  -o -name '*.c' -path '*/external/printf/printf.c' \
  | sort)

# Ищем оба варианта расширения: *.s и *.S
SRCS_S := $(shell find src \( -name '*.s' -o -name '*.S' \) ! -path '*/external/*' | sort)

OBJS := $(patsubst %.c,$(BUILD)/%.o,$(SRCS_C)) \
        $(patsubst %.S,$(BUILD)/%.o,$(filter %.S,$(SRCS_S))) \
        $(patsubst %.s,$(BUILD)/%.o,$(filter %.s,$(SRCS_S)))

all: $(BUILD)/$(TARGET).elf $(BUILD)/$(TARGET).bin $(BUILD)/$(TARGET).hex

$(BUILD)/$(TARGET).elf: $(OBJS)
	@mkdir -p $(dir $@)
	$(CC) $(OBJS) $(LDFLAGS) $(LIBS) -o $@
	$(SIZE) $@

$(BUILD)/$(TARGET).bin: $(BUILD)/$(TARGET).elf
	$(OBJCOPY) -O binary $< $@

$(BUILD)/$(TARGET).hex: $(BUILD)/$(TARGET).elf
	$(OBJCOPY) -O ihex $< $@

$(BUILD)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) -c $(CFLAGS) $< -o $@

# Оба правила нужны: GNU make не делает case-insensitive match
$(BUILD)/%.o: %.S
	@mkdir -p $(dir $@)
	$(CC) -c $(ASFLAGS) $< -o $@

$(BUILD)/%.o: %.s
	@mkdir -p $(dir $@)
	$(CC) -c $(ASFLAGS) $< -o $@

clean:
	rm -rf $(BUILD)

.PHONY: all clean
