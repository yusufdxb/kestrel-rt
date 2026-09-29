# Kestrel-RT build. GNU make, no IDE. `make help` lists targets.
#   make APP=benchmark            firmware image build/<app>/kestrel.{elf,bin,map,lst}
#   make host-test                portable components under ASan/UBSan
#   make lint                     cppcheck + clang-format check
#   make flash APP=benchmark      upload through the proven SWD-assisted bossac path
TOOLCHAIN ?= $(HOME)/.local/opt/arm-gnu-toolchain-13.3.rel1-x86_64-arm-none-eabi/bin/arm-none-eabi-
CC      := $(TOOLCHAIN)gcc
OBJCOPY := $(TOOLCHAIN)objcopy
OBJDUMP := $(TOOLCHAIN)objdump
SIZE    := $(TOOLCHAIN)size
NM      := $(TOOLCHAIN)nm

APP   ?= benchmark
BUILD := build/$(APP)
ELF   := $(BUILD)/kestrel.elf

CPUFLAGS := -mcpu=cortex-m4 -mthumb -mfpu=fpv4-sp-d16 -mfloat-abi=hard
WARN := -Wall -Wextra -Werror -Wconversion -Wshadow -Wpedantic -Wundef -Wdouble-promotion -Wformat=2 \
        -Wstrict-prototypes -Wmissing-prototypes -Wcast-align -Wvla -Wstack-usage=768
GIT_ID := $(shell git rev-parse --short=10 HEAD 2>/dev/null || echo nogit)$(shell git diff --quiet 2>/dev/null || echo +)
CFLAGS := $(CPUFLAGS) -std=c17 -O2 -g3 -ffreestanding -fno-common -ffunction-sections -fdata-sections \
          -fno-strict-aliasing -fno-tree-loop-distribute-patterns -fstack-usage -fno-builtin-malloc -fno-builtin-free \
          $(WARN) -I. -isystem third_party/cmsis/Include -isystem third_party/renesas/Include -Iboard \
          -DKESTREL_BUILD_ID=\"$(GIT_ID)\" $(EXTRA_CFLAGS)
ASFLAGS := $(CPUFLAGS) -g3 -I.
LDFLAGS := $(CPUFLAGS) -nostdlib -nostartfiles -Wl,--gc-sections -Wl,-Map=$(BUILD)/kestrel.map \
           -Wl,--print-memory-usage -Wl,--no-warn-rwx-segments -T linker/kestrel_ra4m1.ld

# Runtime sources (every image). Apps add their own.
KERNEL_SRC := $(sort $(wildcard startup/*.c kernel/*.c kernel/*/*.c safety/*/*.c trace/*.c drivers/*/*.c))
KERNEL_ASM := $(sort $(wildcard arch/armv7m/*.S safety/*/*.S))
APP_SRC := $(sort $(wildcard apps/$(APP)/*.c apps/common/*.c apps/fault_injection/*.c))

OBJS := $(addprefix $(BUILD)/,$(KERNEL_SRC:.c=.o) $(KERNEL_ASM:.S=.o) $(APP_SRC:.c=.o))

.PHONY: all fw bin size disasm flash host-test lint format clean help stack-report
all: fw
fw: $(ELF) $(BUILD)/kestrel.bin $(BUILD)/kestrel.lst size

# Runtime code never touches FP registers: a lazy FP save into an unprivileged task's stack after
# the MPU has been retargeted would fault. Only application code (apps/) may use the FPU.
$(BUILD)/apps/%.o: apps/%.c
	@mkdir -p $(dir $@)
	@echo "  CC  $< (app, FPU allowed)"
	@$(CC) $(CFLAGS) -MMD -MP -c $< -o $@
$(BUILD)/%.o: %.c
	@mkdir -p $(dir $@)
	@echo "  CC  $<"
	@$(CC) $(CFLAGS) -mgeneral-regs-only -MMD -MP -c $< -o $@
$(BUILD)/%.o: %.S
	@mkdir -p $(dir $@)
	@echo "  AS  $<"
	@$(CC) $(ASFLAGS) -MMD -MP -c $< -o $@

$(ELF): $(OBJS) linker/kestrel_ra4m1.ld
	@echo "  LD  $@"
	@$(CC) $(LDFLAGS) $(OBJS) -lgcc -o $@
	python3 scripts/check_image.py $@

$(BUILD)/kestrel.bin: $(ELF)
	$(OBJCOPY) -O binary --only-section=.vectors --only-section=.text --only-section=.rodata \
	  --only-section=.ARM.exidx --only-section=.data --only-section=.app_shared $< $@
	sha256sum $@ > $@.sha256

$(BUILD)/kestrel.lst: $(ELF)
	$(OBJDUMP) -d -S $< > $@

size: $(ELF)
	@$(SIZE) -A -x $(ELF) | grep -E '^\.|section' > $(BUILD)/size.txt; $(SIZE) $(ELF)

disasm: $(BUILD)/kestrel.lst

stack-report: fw
	python3 scripts/stack_report.py $(BUILD)

flash: fw
	scripts/flash.sh $(BUILD)/kestrel.bin

host-test:
	$(MAKE) -C tests

lint:
	scripts/lint.sh

format:
	clang-format -i $$(git ls-files '*.c' '*.h' | grep -v third_party)

clean:
	rm -rf build
	$(MAKE) -C tests clean

help:
	@grep -E '^#   ' Makefile

-include $(OBJS:.o=.d)
