#!/usr/bin/env python3
"""mkfatimg.py — сборка образа ex-nihilo-OS с FAT16.

Раскладка (см. fs/fat16.h):

    сектор 0      boot.asm          (ровно 512 Б)
    сектор 1..4   boot2.asm         (4 сектора)
    сектор 3..63  kernel.bin        (запас под рост ядра)
    сектор 64..   FAT16 на весь остаток образа

Файловую систему создаёт НЕ этот скрипт, а независимая реализация —
pyfatfs. Скрипт лишь раскладывает секторы и кладёт в ФС исходные файлы.
Так драйвер в ядре проверяется против чужой реализации, а не против
самого себя.

Использование:
    python3 tools/mkfatimg.py <boot.bin> <boot2.bin> <kernel.bin> <out.img> \
        [user1.elf user2.elf]

ELF-программы (если переданы) попадут в корень ФС как USER1.ELF и
USER2.ELF — их в рантайме запускает kexecve() из ядра.
"""

import sys

try:
    from pyfatfs.PyFat import PyFat
    from pyfatfs.PyFatFS import PyFatFS
except ImportError:
    sys.exit("mkfatimg: нужен pyfatfs (pip install pyfatfs)")

SECTOR = 512
BOOT2_SECTORS = 4                       # должно совпадать с boot.asm
FAT_BASE_LBA = 64                       # совпадает с Makefile и hal/ata.h
IMAGE_SECTORS = 32768                   # 16 МиБ
IMAGE_BYTES = IMAGE_SECTORS * SECTOR

# Файлы, которые попадут в образ. Содержимое BIG/MEDIA генерируется
# формулой, чтобы тест мог проверить каждый байт.
def seeded_files():
    return {
        "HELLO.TXT": b"hello fat16\n",
        "BIG.BIN": bytes(range(256)) * 12,                        # 3072 Б
        "MEDIA.BIN": bytes(((i * 7 + 13) & 0xFF)
                           for i in range(70000)),                # 35 кластеров
    }


def read_bpb(img, base=0):
    """Разобрать BPB: вернуть словарь с геометрией тома."""
    g16 = lambda o: int.from_bytes(img[base + o:base + o + 2], "little")
    g32 = lambda o: int.from_bytes(img[base + o:base + o + 4], "little")
    reserved, nfats, root_ent = g16(14), img[base + 16], g16(17)
    total = g16(19) or g32(32)
    fatsz, spc = g16(22), img[base + 13]
    root_secs = (root_ent * 32 + 511) // 512
    fat_lba = reserved
    root_lba = fat_lba + nfats * fatsz
    data_lba = root_lba + root_secs
    return dict(img=img, base=base, spc=spc, fatsz=fatsz, fat_lba=fat_lba,
                root_lba=root_lba, root_ent=root_ent, data_lba=data_lba,
                clusters=(total - data_lba) // spc)


def fat_entry(fs, cluster):
    off = fs["fat_lba"] * 512 + cluster * 2
    return int.from_bytes(fs["img"][off:off + 2], "little")


def chain(fs, first):
    out, cur = [], first
    while 2 <= cur < fs["clusters"] + 2:
        out.append(cur)
        nxt = fat_entry(fs, cur)
        if nxt >= 0xFFF8 or nxt == 0:
            break
        cur = nxt
    return out


def dir_entries(fs, lba, count):
    """Записи каталога из непрерывной области (корень)."""
    out = []
    for i in range(count):
        off = lba * 512 + i * 32
        e = fs["img"][off:off + 32]
        if e[0] == 0x00:
            break
        if e[0] == 0xE5 or e[11] == 0x0F or (e[11] & 0x08):
            continue
        out.append((e[:11], e[11], int.from_bytes(e[26:28], "little"),
                    int.from_bytes(e[28:32], "little")))
    return out


def name83_to_str(raw):
    """b"HELLO   TXT" -> "HELLO.TXT" (как name_from_83 в драйвере)."""
    base = raw[:8].decode("latin-1").rstrip(" ")
    ext = raw[8:11].decode("latin-1").rstrip(" ")
    return base + ("." + ext if ext else "")


def entries_to_map(entries):
    """[(name83, attr, cluster, size)] -> {"NAME.EXT": (cluster, size)}."""
    out = {}
    for raw, _attr, cluster, size in entries:
        out[name83_to_str(raw)] = (cluster, size)
    return out


def write_data_clusters(path, seeded):
    """Записать точные байты файлов в их кластеры (в обход pyfatfs)."""
    img = bytearray(open(path, "rb").read())
    fs = read_bpb(img)

    root = dir_entries(fs, fs["root_lba"], fs["root_ent"])
    by_name = entries_to_map(root)

    for name, data in seeded.items():
        parts = name.split("/")
        if len(parts) == 1:
            first, size = by_name[parts[0]]
        else:
            sub_first = by_name[parts[0]][0]
            sub = dir_entries(fs, (fs["data_lba"] + (sub_first - 2) * fs["spc"]),
                              fs["spc"] * 512 // 32)
            sub_by_name = entries_to_map(sub)
            first, size = sub_by_name[parts[1]]

        if size != len(data):
            sys.exit("mkfatimg: %s: размер %d вместо %d" % (name, size, len(data)))

        cl = chain(fs, first)
        need = -(-len(data) // (fs["spc"] * 512))
        if len(cl) < need:
            sys.exit("mkfatimg: %s: цепочка из %d кластеров, нужно %d"
                     % (name, len(cl), need))

        for i, cluster in enumerate(cl):
            lba = fs["data_lba"] + (cluster - 2) * fs["spc"]
            start = i * fs["spc"] * 512
            chunk = data[start:start + fs["spc"] * 512]
            if not chunk:
                break
            img[lba * 512:lba * 512 + len(chunk)] = chunk

    open(path, "wb").write(img)


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

    # 1. Заготовка образа
    img = bytearray(IMAGE_BYTES)
    img[0:SECTOR] = boot
    img[SECTOR:SECTOR + len(boot2)] = boot2
    img[(1 + BOOT2_SECTORS) * SECTOR:(1 + BOOT2_SECTORS) * SECTOR + len(kernel)] = kernel
    open(out_path, "wb").write(img)

    # 2. FAT16 через pyfatfs — на весь остаток образа после FAT_BASE_LBA.
    #    pyfatfs пишет ФС от нулевого смещения файла, поэтому создаём её
    #    отдельно и переносим нужный кусок.
    fs_size = IMAGE_BYTES - FAT_BASE_LBA * SECTOR
    tmp_path = out_path + ".fat"
    open(tmp_path, "wb").write(bytearray(fs_size))

    # mkfs — низкоуровневым API, создание файлов — через обёртку
    # PyFilesystem2: у PyFat.open() другое назначение (открыть сам образ).
    pf = PyFat()
    pf.mkfs(tmp_path, fat_type=PyFat.FAT_TYPE_FAT16,
            size=fs_size, sector_size=SECTOR, label="EXNIHILO")
    pf.close()

    # pyfatfs работает с ТЕКСТОМ: байты >= 0x80 он превращает в два
    # символа, и файл получается другого размера. Поэтому структуру ФС
    # (каталоги, цепочки кластеров, размеры) создаём через pyfatfs
    # заглушками нужной длины, а сами данные кладём в кластеры напрямую.
    seeded = seeded_files()
    seeded["DIR1/NESTED.TXT"] = b"nested\n"
    for i, elf_path in enumerate(user_elfs, 1):
        # Байты ELF кладём как есть: write_data_clusters() запишет их
        # в кластеры напрямую, в обход текстовых ограничений pyfatfs.
        seeded["USER%d.ELF" % i] = open(elf_path, "rb").read()

    fs = PyFatFS(tmp_path)
    for name, data in seeded.items():
        if "/" in name:
            continue                    # сначала каталог, потом файлы в нём
        with fs.open("/" + name, "w") as f:
            f.write("x" * len(data))
    fs.makedir("/DIR1")
    fs.close()

    # файл внутри каталога — во второй сессии: pyfatfs кэширует
    # корневой каталог на момент монтирования
    fs = PyFatFS(tmp_path)
    for name, data in seeded.items():
        if "/" not in name:
            continue
        with fs.open("/" + name, "w") as f:
            f.write("x" * len(data))
    fs.close()

    write_data_clusters(tmp_path, seeded)

    fat = open(tmp_path, "rb").read()
    if len(fat) != fs_size:
        sys.exit("mkfatimg: pyfatfs создал ФС размером %d вместо %d"
                 % (len(fat), fs_size))

    # 3. Вставить ФС в образ
    data = open(out_path, "rb").read()
    out = data[:FAT_BASE_LBA * SECTOR] + fat
    open(out_path, "wb").write(out)

    import os
    os.unlink(tmp_path)

    # 4. Отчитаться о раскладке
    print("image: %d sectors (%d MiB), kernel %d bytes in sectors %d..%d, "
          "FAT16 from sector %d" % (IMAGE_SECTORS, IMAGE_BYTES >> 20,
                                    len(kernel), 1 + BOOT2_SECTORS, kernel_end - 1,
                                    FAT_BASE_LBA))


if __name__ == "__main__":
    main()
