#!/usr/bin/env python3
"""mkfatimg.py — сборка образа ex-nihilo-OS с FAT16.

Раскладка (см. fs/fat16.h):

    сектор 0      boot.asm          (ровно 512 Б)
    сектор 1..4   boot2.asm         (4 сектора)
    сектор 5..63  kernel.bin        (запас под рост ядра)
    сектор 64..   FAT16 на весь остаток образа

Файловую систему создаёт НЕ этот скрипт, а независимая реализация —
mtools (mformat/mmd/mcopy). Скрипт лишь раскладывает секторы,
готовит исходные файлы во временном каталоге и вызывает mtools.
Так драйвер в ядре проверяется против чужого кода, а не против
самого себя. mtools работает с байтами честно (бинарный режим
mcopy), поэтому данные не нужно докладывать в кластеры вручную.

Использование:
    python3 tools/mkfatimg.py <boot.bin> <boot2.bin> <kernel.bin> <out.img> \
        [user1.elf user2.elf]

ELF-программы (если переданы) попадут в корень ФС как USER1.ELF и
USER2.ELF — их в рантайме запускает kexecve() из ядра.

Требуется: mtools (mformat, mmd, mcopy) в PATH.
"""

import os
import shutil
import subprocess
import sys
import tempfile

SECTOR = 512
BOOT2_SECTORS = 4                       # должно совпадать с boot.asm
FAT_BASE_LBA = 128                      # совпадает с Makefile и hal/ata.h
IMAGE_SECTORS = 32768                   # 16 МиБ
IMAGE_BYTES = IMAGE_SECTORS * SECTOR

# Геометрия тома FAT16. Размер ФС — 32704 сектора; при кластере в
# 4 сектора (2048 Б) получается ~8100 кластеров, что честно попадает
# в диапазон FAT16 (4085..65524): тип FAT mformat выберет по числу
# кластеров, а драйвер ядра перепроверит его при монтировании.
# Головки/секторы на дорожку задаём, но такие, на которые размер
# тома делится: 32704 % 32 == 0. Классические 63 сектора размер не
# делят, и mtools справедливо ругается при следующих же вызовах.
# Саму геометрию CHS в этой системе никто не использует (ядро и
# host-тесты ходят чистым LBA по BPB).
FS_SECTORS = IMAGE_SECTORS - FAT_BASE_LBA
CLUSTER_SECTORS = 4
HEADS = 16
SECTORS_PER_TRACK = 32
VOLUME_LABEL = "EXNIHILO"


# Файлы, которые попадут в образ. Содержимое BIG/MEDIA генерируется
# формулой, чтобы тест мог проверить каждый байт.
def seeded_files():
    return {
        "HELLO.TXT": b"hello fat16\n",
        "BIG.BIN": bytes(range(256)) * 12,                        # 3072 Б
        "MEDIA.BIN": bytes(((i * 7 + 13) & 0xFF)
                           for i in range(70000)),                # 35 кластеров
        "DIR1/NESTED.TXT": b"nested\n",
    }


def run_mtool(tool, args):
    """Вызвать mtools, при ошибке — внятно упасть с её выводом."""
    if shutil.which(tool) is None:
        sys.exit("mkfatimg: нужен mtools (%s не найден в PATH; "
                 "sudo apt-get install mtools)" % tool)
    proc = subprocess.run([tool] + args, capture_output=True, text=True)
    if proc.returncode != 0:
        sys.exit("mkfatimg: %s %s упал (%d):\n%s%s"
                 % (tool, " ".join(args), proc.returncode,
                    proc.stdout, proc.stderr))
    return proc


def build_filesystem(fs_path, seeded):
    """Создать FAT16 в fs_path и положить туда seeded через mtools."""
    with open(fs_path, "wb") as f:
        f.write(bytearray(FS_SECTORS * SECTOR))

    # -v: метка тома; -T: размер в секторах; -h/-s: геометрия;
    # -c: секторов на кластер.
    run_mtool("mformat", ["-i", fs_path,
                          "-T", str(FS_SECTORS),
                          "-h", str(HEADS),
                          "-s", str(SECTORS_PER_TRACK),
                          "-c", str(CLUSTER_SECTORS),
                          "-v", VOLUME_LABEL, "::"])

    with tempfile.TemporaryDirectory(prefix="mkfatimg-") as stage:
        dirs = set()
        for name in seeded:
            if "/" in name:
                dirs.add(name.split("/")[0])
        for d in sorted(dirs):
            run_mtool("mmd", ["-i", fs_path, "::" + d])

        for name, data in seeded.items():
            host = os.path.join(stage, name.replace("/", "_"))
            with open(host, "wb") as f:
                f.write(data)
            # mcopy по умолчанию бинарный (текстовый режим — только -t),
            # так что ELF и паттерны >= 0x80 доедут байт в байт.
            run_mtool("mcopy", ["-i", fs_path, host, "::" + name])

    got = os.path.getsize(fs_path)
    if got != FS_SECTORS * SECTOR:
        sys.exit("mkfatimg: mformat создал ФС размером %d вместо %d"
                 % (got, FS_SECTORS * SECTOR))


def main():
    if len(sys.argv) < 5:
        sys.exit(__doc__.strip().splitlines()[-1])

    boot_path, boot2_path, kernel_path, out_path = sys.argv[1:5]
    user_elfs = sys.argv[5:]            # [user1.elf, user2.elf] -> USERn.ELF
    boot = open(boot_path, "rb").read()
    boot2 = open(boot2_path, "rb").read()
    kernel = open(kernel_path, "rb").read()

    if len(boot) != SECTOR:
        sys.exit("mkfatimg: boot.bin должен быть ровно 512 байт")
    if len(boot2) != BOOT2_SECTORS * SECTOR:
        sys.exit("mkfatimg: boot2.bin должен занимать %d сектора" % BOOT2_SECTORS)

    kernel_sectors = -(-len(kernel) // SECTOR)
    kernel_end = 1 + BOOT2_SECTORS + kernel_sectors
    if kernel_end > FAT_BASE_LBA:
        sys.exit("mkfatimg: ядро (%d секторов) залезает в FAT16 — "
                 "начните ФС позже или уменьшите ядро" % kernel_sectors)

    seeded = seeded_files()
    for elf_path in user_elfs:
        name = os.path.basename(elf_path).upper()
        seeded[name] = open(elf_path, "rb").read()

    # 1. FAT16 через mtools — отдельным файлом: mtools пишет ФС от
    #    нулевого смещения, а в образе ей положено жить с сектора 64.
    tmp_path = out_path + ".fat"
    try:
        build_filesystem(tmp_path, seeded)
        fat = open(tmp_path, "rb").read()
    finally:
        if os.path.exists(tmp_path):
            os.unlink(tmp_path)

    # 2. Заготовка образа: boot + boot2 + ядро + FAT16 с сектора 64.
    img = bytearray(IMAGE_BYTES)
    img[0:SECTOR] = boot
    img[SECTOR:SECTOR + len(boot2)] = boot2
    img[(1 + BOOT2_SECTORS) * SECTOR:(1 + BOOT2_SECTORS) * SECTOR + len(kernel)] = kernel
    img[FAT_BASE_LBA * SECTOR:FAT_BASE_LBA * SECTOR + len(fat)] = fat
    open(out_path, "wb").write(img)

    # 3. Отчитаться о раскладке
    print("image: %d sectors (%d MiB), kernel %d bytes in sectors %d..%d, "
          "FAT16 from sector %d" % (IMAGE_SECTORS, IMAGE_BYTES >> 20,
                                    len(kernel), 1 + BOOT2_SECTORS, kernel_end - 1,
                                    FAT_BASE_LBA))


if __name__ == "__main__":
    main()
