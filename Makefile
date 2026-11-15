# Makefile — сборка ex-nihilo-OS
#
#   make            — собрать build/os.img (образ дискеты: boot + kernel)
#   make run        — запустить образ в QEMU
#   make test       — прогнать host-тесты PMM (настоящий hal/pmm.asm)
#   make clean      — удалить build/
#
# Требуется: nasm, gcc (с поддержкой -m32), binutils-ld, для `make run` — qemu-system-i386.

NASM        ?= nasm
CC          ?= gcc
LD          ?= ld

BUILD       := build
BOOT_BIN    := $(BUILD)/boot.bin
KERNEL_BIN  := $(BUILD)/kernel.bin
IMAGE       := $(BUILD)/os.img

CFLAGS      := -m32 -ffreestanding -fno-pic -fno-pie -fno-stack-protector -fno-builtin \
               -fno-asynchronous-unwind-tables -nostdlib -std=gnu11 -O2 \
               -Wall -Wextra -Werror -I. -Ilibc

LDFLAGS     := -m elf_i386 -nostdlib -T linker.ld --build-id=none

C_SRCS      := kernel.c
ASM_SRCS    := kernel.asm hal/pmm.asm hal/spinlock.asm

# .c и .asm могут иметь одинаковое имя (kernel.c / kernel.asm),
# поэтому объектники раскладываем по разным каталогам.
C_OBJS      := $(patsubst %.c,$(BUILD)/c/%.o,$(C_SRCS))
ASM_OBJS    := $(patsubst %.asm,$(BUILD)/asm/%.o,$(ASM_SRCS))

TEST_DIR    := tests
TEST_BIN    := $(BUILD)/pmm_test

.PHONY: all run test clean

all: $(IMAGE)

# --- загрузочный сектор ---
$(BOOT_BIN): boot.asm
	@mkdir -p $(BUILD)
	$(NASM) -f bin $< -o $@

# --- объектники ядра ---
$(BUILD)/asm/%.o: %.asm
	@mkdir -p $(dir $@)
	$(NASM) -f elf32 $< -o $@

$(BUILD)/c/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

# --- плоский образ ядра (linker.ld: OUTPUT_FORMAT("binary"), точка входа 0x10000) ---
$(KERNEL_BIN): $(ASM_OBJS) $(C_OBJS) linker.ld
	$(LD) $(LDFLAGS) -o $@ $(ASM_OBJS) $(C_OBJS)
	@test `stat -c%s $@` -le 32768 || \
	  (echo "error: kernel.bin exceeds the 32 KiB the bootloader loads" && rm -f $@ && exit 1)
	@echo "built $@ (`stat -c%s $@` bytes)"

# --- образ дискеты 1.44 МБ: сектор 0 — загрузчик, дальше — ядро ---
$(IMAGE): $(BOOT_BIN) $(KERNEL_BIN)
	dd if=/dev/zero of=$@ bs=512 count=2880 status=none
	dd if=$(BOOT_BIN)   of=$@ bs=512 conv=notrunc status=none
	dd if=$(KERNEL_BIN) of=$@ bs=512 seek=1 conv=notrunc status=none
	@echo "built $@ (`stat -c%s $@` bytes)"

run: $(IMAGE)
	qemu-system-i386 -drive format=raw,file=$(IMAGE),if=floppy -boot a

# --- host-тест PMM: настоящие hal/pmm.asm + hal/spinlock.asm ---
$(BUILD)/tests/start.o: $(TEST_DIR)/start.asm
	@mkdir -p $(dir $@)
	$(NASM) -f elf32 $< -o $@

$(BUILD)/tests/pmm_test.o: $(TEST_DIR)/pmm_test.c hal/pmm.h libc/stdint.h libc/stddef.h
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/tests/pmm_under_test.o: hal/pmm.asm
	@mkdir -p $(dir $@)
	$(NASM) -f elf32 $< -o $@

$(BUILD)/tests/spinlock.o: hal/spinlock.asm
	@mkdir -p $(dir $@)
	$(NASM) -f elf32 $< -o $@

$(TEST_BIN): $(BUILD)/tests/start.o $(BUILD)/tests/pmm_test.o \
             $(BUILD)/tests/pmm_under_test.o $(BUILD)/tests/spinlock.o $(TEST_DIR)/test.ld
	$(LD) -m elf_i386 -nostdlib -T $(TEST_DIR)/test.ld -o $@ $(filter %.o,$^)

test: $(TEST_BIN)
	./$(TEST_BIN)

clean:
	rm -rf $(BUILD)
