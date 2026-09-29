# Baseline images: make -f baselines/baseline.mk BASE=freertos|superloop
# Reuses Kestrel's startup, clocks, register-level drivers and frame codec; no Kestrel kernel.
TOOLCHAIN ?= $(HOME)/.local/opt/arm-gnu-toolchain-13.3.rel1-x86_64-arm-none-eabi/bin/arm-none-eabi-
CC := $(TOOLCHAIN)gcc
OBJCOPY := $(TOOLCHAIN)objcopy
SIZE := $(TOOLCHAIN)size
BASE ?= freertos
BUILD := build/base_$(BASE)
CPU := -mcpu=cortex-m4 -mthumb -mfpu=fpv4-sp-d16 -mfloat-abi=hard
CFLAGS := $(CPU) -std=c17 -O2 -g3 -ffreestanding -fno-common -ffunction-sections -fdata-sections \
  -fno-tree-loop-distribute-patterns -Wall -Wextra -Werror -I. -Iboard -isystem third_party/cmsis/Include \
  -isystem third_party/renesas/Include -DKESTREL_BUILD_ID=\"base\"
LDFLAGS := $(CPU) -nostdlib -nostartfiles -Wl,--gc-sections -Wl,-Map=$(BUILD)/image.map \
  -Wl,--print-memory-usage -Wl,--no-warn-rwx-segments -T linker/kestrel_ra4m1.ld
COMMON := startup/startup.c startup/clock.c kernel/memory/kstring.c drivers/icu/icu.c drivers/uart/sci_uart.c \
  drivers/gpio/gpio.c drivers/adc/adc.c trace/frame.c trace/crc16.c apps/common/workload.c baselines/common/bench.c
FR := build/ext/FreeRTOS-Kernel
FRSRC := $(FR)/tasks.c $(FR)/list.c $(FR)/queue.c $(FR)/portable/GCC/ARM_CM4F/port.c
ifeq ($(BASE),freertos)
SRC := $(COMMON) baselines/freertos/main.c $(FRSRC)
CFLAGS += -Ibaselines/freertos -I$(FR)/include -I$(FR)/portable/GCC/ARM_CM4F
else ifeq ($(BASE),freertos_ctx)
SRC := $(COMMON) baselines/freertos/ctx_main.c $(FRSRC)
CFLAGS += -Ibaselines/freertos -I$(FR)/include -I$(FR)/portable/GCC/ARM_CM4F
else
SRC := $(COMMON) baselines/superloop/main.c
endif
OBJS := $(addprefix $(BUILD)/,$(SRC:.c=.o))

all: $(BUILD)/image.bin
$(BUILD)/%.o: %.c
	@mkdir -p $(dir $@)
	@echo "  CC  $<"
	@$(CC) $(CFLAGS) -c $< -o $@
$(BUILD)/image.elf: $(OBJS)
	@$(CC) $(LDFLAGS) $(OBJS) -lgcc -o $@
	@python3 scripts/check_image.py $@
$(BUILD)/image.bin: $(BUILD)/image.elf
	@$(OBJCOPY) -O binary --only-section=.vectors --only-section=.text --only-section=.rodata \
	  --only-section=.ARM.exidx --only-section=.data --only-section=.app_shared $< $@
	@$(SIZE) -A $< | grep -E "^\.(text|rodata|data|bss|task_stacks|app_shared) " ; $(SIZE) $<
