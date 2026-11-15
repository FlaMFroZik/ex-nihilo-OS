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
BOOT2_BIN   := $(BUILD)/boot2.bin
KERNEL_BIN  := $(BUILD)/kernel.bin

# Раскладка образа (в секторах по 512 Б):
#   0                   — первый этап (boot.asm, ровно 512 Б)
#   1 .. STAGE2_SECTORS — второй этап (boot2.asm)
#   дальше              — ядро (kernel.bin)
# Значения обязаны совпадать с константами в boot.asm/boot2.asm;
# проверка соответствия — targets/layout-check в `make test`.
STAGE2_SECTORS := 2
KERNEL_LBA     := $(shell expr 1 + $(STAGE2_SECTORS))
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
BOOTEMU     := tools/bootemu.py

# unicorn нужен только для эмуляционной проверки загрузчика.
# Если его нет — цель пропускается с предупреждением, а не падает.
PYTHON      ?= python3
HAVE_UNICORN := $(shell $(PYTHON) -c "import unicorn" 2>/dev/null && echo 1)

.PHONY: all run test boottest clean

all: $(IMAGE)

# --- загрузочный сектор ---
$(BOOT_BIN): boot.asm
	@mkdir -p $(BUILD)
	$(NASM) -f bin $< -o $@

# --- второй этап загрузки ---
$(BOOT2_BIN): boot2.asm
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

# --- образ дискеты 1.44 МБ ---
$(IMAGE): $(BOOT_BIN) $(BOOT2_BIN) $(KERNEL_BIN)
	dd if=/dev/zero of=$@ bs=512 count=2880 status=none
	dd if=$(BOOT_BIN)   of=$@ bs=512 conv=notrunc status=none
	dd if=$(BOOT2_BIN)  of=$@ bs=512 seek=1 conv=notrunc status=none
	dd if=$(KERNEL_BIN) of=$@ bs=512 seek=$(KERNEL_LBA) conv=notrunc status=none
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

# Адреса карты E820 и ячейки BIOS переносим в .bss теста: в user
# space 0x600/0x413 не отображены. Значения совпадают с tests/start.asm.
TEST_E820_DEFS := -DPMM_HOST_TEST \
                  -DE820_COUNT_ADDR=test_e820_count \
                  -DE820_BUF_ADDR=test_e820_buf \
                  -DBIOS_MEM_KB_ADDR=test_bios_mem_kb

$(BUILD)/tests/pmm_under_test.o: hal/pmm.asm
	@mkdir -p $(dir $@)
	$(NASM) -f elf32 $(TEST_E820_DEFS) $< -o $@

$(BUILD)/tests/spinlock.o: hal/spinlock.asm
	@mkdir -p $(dir $@)
	$(NASM) -f elf32 $< -o $@

$(TEST_BIN): $(BUILD)/tests/start.o $(BUILD)/tests/pmm_test.o \
             $(BUILD)/tests/pmm_under_test.o $(BUILD)/tests/spinlock.o $(TEST_DIR)/test.ld
	$(LD) -m elf_i386 -nostdlib -T $(TEST_DIR)/test.ld -o $@ $(filter %.o,$^)

test: $(TEST_BIN)
	./$(TEST_BIN)

# --- проверка раскладки образа (совпадение констант Makefile и asm) ---
layout-check: $(IMAGE)
	@test "$$(stat -c%s $(BOOT_BIN))" -eq 512 || \
	  (echo "error: boot.bin должен быть ровно 512 байт" && exit 1)
	@test "$$(stat -c%s $(BOOT2_BIN))" -eq $$((512 * $(STAGE2_SECTORS))) || \
	  (echo "error: boot2.bin не совпадает с STAGE2_SECTORS" && exit 1)
	@# первый сектор ядра в образе обязан совпасть с первым сектором kernel.bin
	@dd if=$(IMAGE) bs=512 skip=$(KERNEL_LBA) of=$(BUILD)/img_k.bin \
	  count=1 status=none
	@dd if=$(KERNEL_BIN) of=$(BUILD)/ker_k.bin bs=512 count=1 status=none
	@cmp -s $(BUILD)/img_k.bin $(BUILD)/ker_k.bin || \
	  (echo "error: ядро в образе не на месте KERNEL_LBA=$(KERNEL_LBA)" && exit 1)
	@echo "layout ok: boot=сектор 0, boot2=1..$(STAGE2_SECTORS), kernel=$(KERNEL_LBA).."

# --- эмуляционная проверка загрузчика (нужен unicorn) ---
boottest: $(IMAGE) $(KERNEL_BIN)
ifdef HAVE_UNICORN
	$(PYTHON) $(BOOTEMU) $(IMAGE) $(KERNEL_BIN)
else
	@echo "boottest: пропущено — не установлен unicorn (pip install unicorn)"
endif

clean:
	rm -rf $(BUILD)
