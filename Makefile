TARGET  := rt880_blink
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
  -Isrc/external/drivers/inc

CFLAGS := $(MCUFLAGS) -ffunction-sections -fdata-sections -Wall -Wextra -Os -g3 $(C_DEFS) $(C_INCLUDES)
ASFLAGS := $(MCUFLAGS) -x assembler-with-cpp -g3 $(C_DEFS) $(C_INCLUDES)
LDFLAGS := $(MCUFLAGS) -Tlinker.ld -Wl,--gc-sections -Wl,-Map=$(BUILD)/$(TARGET).map --specs=nano.specs --specs=nosys.specs
LIBS := -lc -lm -lnosys

SRCS_C := \
  src/main.c \
  src/driver/rt880_board.c \
  src/external/cmsis/cm4/device_support/system_at32f423.c \
  src/external/drivers/src/at32f423_crm.c \
  src/external/drivers/src/at32f423_gpio.c \
  src/external/drivers/src/at32f423_misc.c

SRCS_S := \
  src/helper/startup_at32f423.s

OBJS := $(patsubst %.c,$(BUILD)/%.o,$(SRCS_C)) \
        $(patsubst %.S,$(BUILD)/%.o,$(SRCS_S))

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

$(BUILD)/%.o: %.S
	@mkdir -p $(dir $@)
	$(CC) -c $(ASFLAGS) $< -o $@

clean:
	rm -rf $(BUILD)

.PHONY: all clean
