# Makefile — сборка ex-nihilo-OS
#
#   make            — собрать build/os.img (образ HDD: boot + kernel + FAT16)
#   make run        — запустить образ в QEMU (как жёсткий диск)
#   make test       — host-тесты PMM и FAT16 (настоящие hal/pmm.asm, fs/fat16.c)
#   make boottest   — эмуляционная проверка загрузчика (нужен unicorn)
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
#   1 .. STAGE2_SECTORS — второй этап (boot2.asm), 4 сектора
#   дальше              — ядро (kernel.bin)
# Значения обязаны совпадать с константами в boot.asm/boot2.asm;
# проверка соответствия — targets/layout-check в `make test`.
STAGE2_SECTORS := 4
KERNEL_LBA     := $(shell expr 1 + $(STAGE2_SECTORS))
IMAGE       := $(BUILD)/os.img
# FAT16 занимает весь образ после этого сектора. 64 = 1 (boot)
# + 2 (boot2) + 61 сектор запаса под ядро (ядро сейчас ~32 сектора).
# Должно совпадать с FAT_BASE_LBA в hal/ata.h и fs/fat16.h.
FAT_BASE_LBA := 64

CFLAGS      := -m32 -ffreestanding -fno-pic -fno-pie -fno-stack-protector -fno-builtin \
               -fno-asynchronous-unwind-tables -nostdlib -std=gnu11 -O2 \
               -Wall -Wextra -Werror -I. -Ilibc

LDFLAGS     := -m elf_i386 -nostdlib -T linker.ld --build-id=none

C_SRCS      := kernel.c kernel/kapi.c kernel/syscall.c kernel/user.c kernel/exec.c kernel/paging.c \
               fs/fat16.c hal/ata.c hal/video.c hal/serial.c
ASM_SRCS    := kernel.asm kernel/syscall.asm hal/pmm.asm hal/spinlock.asm hal/ata.asm
FS_C_SRCS   := fs/fat16.c

# .c и .asm могут иметь одинаковое имя (kernel.c / kernel.asm),
# поэтому объектники раскладываем по разным каталогам.
C_OBJS      := $(patsubst %.c,$(BUILD)/c/%.o,$(C_SRCS))
ASM_OBJS    := $(patsubst %.asm,$(BUILD)/asm/%.o,$(ASM_SRCS))

TEST_DIR    := tests
TEST_BIN    := $(BUILD)/pmm_test
FAT_TEST    := $(BUILD)/fat_test
BOOTEMU     := tools/bootemu.py
MKFATIMG    := tools/mkfatimg.py

# unicorn нужен только для эмуляционной проверки загрузчика.
# Если его нет — цель пропускается с предупреждением, а не падает.
PYTHON      ?= python3
HAVE_UNICORN := $(shell $(PYTHON) -c "import unicorn" 2>/dev/null && echo 1)
HAVE_MTOOLS  := $(shell command -v mformat >/dev/null && command -v mcopy >/dev/null && echo 1)

.PHONY: all run test fattest boottest layout-check clean

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

# --- пользовательские ELF-программы (programs/): отдельные бинарники ---
# Это НЕ плоский образ: файл обязан остаться ELF, на диск он попадёт
# как USER1.ELF / USER2.ELF, а загрузит его уже kexecve() в ядре.
USER_ELFS   := $(BUILD)/user1.elf $(BUILD)/user2.elf

$(BUILD)/user%.elf: $(BUILD)/asm/programs/start.o $(BUILD)/c/programs/user%.o programs/user.ld
	$(LD) -m elf_i386 -nostdlib -T programs/user.ld -o $@ \
		$(BUILD)/asm/programs/start.o $(BUILD)/c/programs/user$*.o
	@echo "built $@ (`stat -c%s $@` bytes)"

# --- плоский образ ядра (linker.ld: OUTPUT_FORMAT("binary"), точка входа 0x10000) ---
$(KERNEL_BIN): $(ASM_OBJS) $(C_OBJS) linker.ld
	$(LD) $(LDFLAGS) -o $@ $(ASM_OBJS) $(C_OBJS)
	@test `stat -c%s $@` -le 32768 || \
	  (echo "error: kernel.bin exceeds the 32 KiB the bootloader loads" && rm -f $@ && exit 1)
	@echo "built $@ (`stat -c%s $@` bytes)"

# --- образ HDD 16 МиБ: boot + boot2 + kernel + FAT16 ---
#
# Образ именно жёсткого диска, а не дискеты: FAT16 требует не меньше
# 4085 кластеров (иначе это FAT12 по спецификации), а на 1.44 МБ их
# максимум ~2880. Кроме того, ядру нужен драйвер диска в защищённом
# режиме — ATA PIO на HDD проще и надёжнее, чем FDC на дискете.
$(IMAGE): $(BOOT_BIN) $(BOOT2_BIN) $(KERNEL_BIN) $(USER_ELFS) $(MKFATIMG)
ifdef HAVE_MTOOLS
	$(PYTHON) $(MKFATIMG) $(BOOT_BIN) $(BOOT2_BIN) $(KERNEL_BIN) $@ $(USER_ELFS)
	@echo "built $@ (`stat -c%s $@` bytes)"
else
	@echo "error: для сборки образа нужен mtools (sudo apt-get install mtools)"
	@false
endif

run: $(IMAGE)
	# -vga std: QEMU отдаёт через VBE 1024x768x32, ядро рисует отчёт
	# в две колонки. -serial stdio: весь вывод ещё и в консоль терминала.
	qemu-system-i386 -drive format=raw,file=$(IMAGE),if=ide,index=0 -boot c \
		-vga std -serial stdio

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

test: $(TEST_BIN) fattest
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

# --- эмуляционная проверка загрузчика И ядра (нужен unicorn) ---
#
# Единственный тест, который проходит весь путь: BIOS → реальный режим
# → защищённый режим → ATA PIO → FAT16. Ловит то, что не видно ни
# host-тестам, ни сборке: неверную геометрию CHS, неверные порты ATA,
# испорченные аргументы вызовов.
boottest: $(IMAGE) $(KERNEL_BIN)
ifdef HAVE_UNICORN
	$(PYTHON) $(BOOTEMU) $(IMAGE) $(KERNEL_BIN)
else
	@echo "boottest: пропущено — не установлен unicorn (pip install unicorn)"
endif

# --- host-тест драйвера FAT16 ---
#
# Тест линкуется с настоящим fs/fat16.c и работает по образу, который
# собирает mkfatimg.py: структуру ФС создаёт независимая реализация
# (mtools), поэтому драйвер проверяется против чужого кода, а не
# против самого себя.
$(BUILD)/tests/fat_start.o: $(TEST_DIR)/fat_start.asm
	@mkdir -p $(dir $@)
	$(NASM) -f elf32 $< -o $@

$(BUILD)/tests/fat_test.o: $(TEST_DIR)/fat_test.c fs/fat16.h fs/blockdev.h
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/tests/fat16_under_test.o: fs/fat16.c fs/fat16.h fs/blockdev.h
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c fs/fat16.c -o $@

$(FAT_TEST): $(BUILD)/tests/fat_start.o $(BUILD)/tests/fat_test.o \
             $(BUILD)/tests/fat16_under_test.o $(TEST_DIR)/test.ld
	$(LD) -m elf_i386 -nostdlib -T $(TEST_DIR)/test.ld -o $@ $(filter %.o,$^)

fattest: $(FAT_TEST) $(IMAGE)
	./$(FAT_TEST) $(IMAGE)

clean:
	rm -rf $(BUILD)
