ROOT    := ..
BUILD   := $(ROOT)/build
PREFIX  := arm-none-eabi
CC      := $(PREFIX)-gcc
OBJCOPY := $(PREFIX)-objcopy
SIZE    := $(PREFIX)-size

MCUFLAGS := -mcpu=cortex-m4 -mthumb -mfloat-abi=soft
DEFS     := -DAT32F423RCT7 -DUSE_STDPERIPH_DRIVER -DFLASH_MODULE_ENABLED
INCLUDES := -I$(ROOT)/inc -I$(ROOT)/src \
            -I$(ROOT)/src/external/cmsis/cm4/core_support \
            -I$(ROOT)/src/external/cmsis/cm4/device_support \
            -I$(ROOT)/src/external/drivers/inc \
            -I$(ROOT)/src/helper

CFLAGS  := $(MCUFLAGS) -ffunction-sections -fdata-sections -Os -g3 -Wall -Wextra \
           $(DEFS) $(INCLUDES)
ASFLAGS := $(MCUFLAGS) -x assembler-with-cpp -g3 $(DEFS) $(INCLUDES)
LDFLAGS := $(MCUFLAGS) -Tboot2.ld -Wl,--gc-sections -Wl,-Map=$(BUILD)/boot2.map \
           --specs=nano.specs

SRCS_C := boot2.c \
          $(ROOT)/src/external/cmsis/cm4/device_support/system_at32f423.c \
          $(ROOT)/src/external/drivers/src/at32f423_crm.c \
          $(ROOT)/src/external/drivers/src/at32f423_flash.c \
          $(ROOT)/src/external/drivers/src/at32f423_gpio.c \
          $(ROOT)/src/external/drivers/src/at32f423_usart.c
SRCS_S := $(ROOT)/src/helper/startup_at32f423.s

OBJS := $(patsubst %.c,$(BUILD)/boot2/%.o,$(notdir $(SRCS_C))) \
        $(patsubst %.s,$(BUILD)/boot2/%.o,$(notdir $(SRCS_S)))

all: $(BUILD)/boot2.bin

$(BUILD)/boot2.bin: $(BUILD)/boot2.elf
	$(OBJCOPY) -O binary $< $@

$(BUILD)/boot2.elf: $(OBJS)
	$(CC) $(OBJS) $(LDFLAGS) -o $@
	$(SIZE) $@

$(BUILD)/boot2/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) -c -MMD -MP $(CFLAGS) $< -o $@

$(BUILD)/boot2/%.o: $(ROOT)/src/helper/%.s
	@mkdir -p $(dir $@)
	$(CC) -c $(ASFLAGS) $< -o $@

$(BUILD)/boot2/%.o: $(ROOT)/src/external/drivers/src/%.c
	@mkdir -p $(dir $@)
	$(CC) -c -MMD -MP $(CFLAGS) $< -o $@

$(BUILD)/boot2/%.o: $(ROOT)/src/external/cmsis/cm4/device_support/%.c
	@mkdir -p $(dir $@)
	$(CC) -c -MMD -MP $(CFLAGS) $< -o $@

-include $(OBJS:.o=.d)

clean:
	rm -rf $(BUILD)/boot2 $(BUILD)/boot2.elf $(BUILD)/boot2.bin $(BUILD)/boot2.map

.PHONY: all clean
