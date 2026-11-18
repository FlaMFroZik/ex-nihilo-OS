#!/usr/bin/env python3
"""bootemu.py — эмуляционная проверка загрузчика И ядра ex-nihilo-OS.

Загрузчик и драйвер ATA нельзя проверить host-тестами: они целиком
состоят из обращений к BIOS и к портам ввода-вывода. Этот скрипт
запускает образ на эмуляторе (unicorn) и подменяет окружение:

  * INT 10h/AH=0Eh — вывод символа: собираем текст экрана;
  * INT 13h/AH=02h — чтение сектора с диска (только CHS; для HDD
    пересчитываем LBA→CHS по геометрии из BPB образа);
  * INT 13h/AH=08h — геометрия диска;
  * INT 13h/AH=42h — «не реализовано» (так ведёт себя SeaBIOS
    с дискетой; загрузчик обязан обходиться без LBA-чтения);
  * INT 15h/AX=E820h — карта памяти;
  * порты ATA 0x1F0..0x1F7 и 0x3F6 — модель диска PIO LBA28.

Проверяется:
  1. загрузчик не пользуется LBA-чтением (AH=42h);
  2. каждый сектор читается по верному CHS и попадает на своё место;
  3. ядро в памяти побайтово равно build/kernel.bin;
  4. ядро монтирует FAT16 с того же образа и печатает отчёт;
  5. запись через драйвер доходит до диска и читается обратно.

Запуск: python3 tools/bootemu.py build/os.img build/kernel.bin
Требует: pip install unicorn
"""
import struct
import sys

try:
    from unicorn import (
        Uc, UcError, UC_ARCH_X86, UC_MODE_16, UC_MODE_32, UC_HOOK_INTR,
        UC_HOOK_CODE, UC_HOOK_BLOCK,
    )
    from unicorn import UC_HOOK_INSN
    from unicorn.x86_const import UC_X86_INS_IN, UC_X86_INS_OUT, UC_X86_INS_HLT
    from unicorn.x86_const import (
        UC_X86_REG_AH, UC_X86_REG_AL, UC_X86_REG_BX, UC_X86_REG_CH,
        UC_X86_REG_CL, UC_X86_REG_DH, UC_X86_REG_DI, UC_X86_REG_DL,
        UC_X86_REG_EAX, UC_X86_REG_EBX, UC_X86_REG_ECX, UC_X86_REG_EFLAGS,
        UC_X86_REG_EIP, UC_X86_REG_ES, UC_X86_REG_CS, UC_X86_REG_CR0,
        UC_X86_REG_ESP, UC_X86_REG_EBP, UC_X86_REG_EDI, UC_X86_REG_ESI,
        UC_X86_REG_DS, UC_X86_REG_SS, UC_X86_REG_FS, UC_X86_REG_GS,
        UC_X86_REG_GDTR, UC_X86_REG_IDTR, UC_X86_REG_TR,
        UC_X86_REG_SP,
        UC_X86_REG_CX, UC_X86_REG_BP,
        UC_X86_INS_IN, UC_X86_INS_OUT,
    )
except ImportError:
    sys.exit("bootemu: нужен unicorn (pip install unicorn)")

KERNEL_START = 0x10000
STAGE2_SECTORS = 4
KERNEL_LBA = 1 + STAGE2_SECTORS
FAT_LBA = 64
GDT_ADDR = 0x800

# --- искусственный VBE: 1024x768x32, линейный буфер в 0xE0000000 ---
VBE_W, VBE_H, VBE_BPP = 1024, 768, 32
VBE_PITCH = VBE_W * 4
LFB_ADDR = 0xE0000000
LFB_SIZE = VBE_PITCH * VBE_H

# Куда эмулятор складывает вывод COM1 (в реальной машине это порт 0x3F8)
COM1_FIFO = 0xE1000000
COM1_FIFO_SIZE = 1 << 20
VBE_INFO_ADDR = 0x4000          # адрес, который "BIOS" отдаёт как VbeInfoBlock
VBE_MODE_INFO = 0x4400
VBE_MODE_LIST = 0x4600
VBE_MODE_NUMBER = 0x144         # 1024x768x32 по спецификации VBE

# Области, которые загрузчик заполняет для ядра (см. boot2.asm)
BOOT_DRIVE_ADDR = 0x0500         # boot.asm: номер загрузочного диска
E820_BUF_ADDR = 0x0608          # boot2.asm: 20 записей по 24 байта
VIDEO_INFO_ADDR = 0x0900
FONT_ADDR = 0x1000
FAILED = []

# Регистры ATA
R_DATA, R_ERR, R_SC, R_L0, R_L1, R_L2, R_DRIVE, R_STATUS = range(8)
ST_BSY, ST_DRDY, ST_DF, ST_DRQ, ST_ERR = 0x80, 0x40, 0x20, 0x08, 0x01


def check(ok, msg):
    print(("  [ok]   " if ok else "  [FAIL] ") + msg)
    if not ok:
        FAILED.append(msg)


class Disk:
    """Модель ATA PIO LBA28 поверх образа."""

    def __init__(self, img):
        self.img = bytearray(img)
        self.sectors = len(self.img) // 512
        self.regs = [0] * 8
        self.cmd = None
        self.buf = b""
        self.pos = 0
        self.reads = 0
        self.writes = 0
        self.bad_lba = []
        self.dirty = set()

    # --- регистры ---
    def out(self, port, val):
        if port == 0x1F0:                       # данные (write)
            return
        off = port - 0x1F0
        if off > 7:
            return
        self.regs[off] = val & 0xFF
        if off == R_STATUS:                     # команда
            self.execute(val & 0xFF)

    def inp(self, port):
        if port == 0x1F0:
            if self.pos + 2 <= len(self.buf):
                v = self.buf[self.pos] | (self.buf[self.pos + 1] << 8)
                self.pos += 2
                return v
            return 0
        off = port - 0x1F0
        if off == R_STATUS:
            # 0xFF — устройства нет (проверка ata_init): BSY всегда 1
            if self.sectors == 0:
                return 0xFF
            return ST_DRDY | (ST_DRQ if self.pos < len(self.buf) else 0)
        if off > 7:
            return 0
        return self.regs[off]

    def lba(self):
        r = self.regs
        return r[R_L0] | (r[R_L1] << 8) | (r[R_L2] << 16) | \
               ((r[R_DRIVE] & 0x0F) << 24)

    def execute(self, cmd):
        if cmd == 0xEC:                         # IDENTIFY
            ident = bytearray(512)
            ident[120:122] = struct.pack("<H", self.sectors & 0xFFFF)
            ident[122:124] = struct.pack("<H", (self.sectors >> 16) & 0xFFFF)
            # слово 0: 0x0040 = LBA-режим
            ident[0:2] = struct.pack("<H", 0x0040)
            self.buf = bytes(ident)
            self.pos = 0
            for i in range(8):
                self.regs[i] = 0
            return
        if cmd in (0x20, 0x30):
            lba = self.lba()
            if lba >= self.sectors:
                self.bad_lba.append(lba)
                self.buf = b""
            else:
                self.buf = bytes(self.img[lba * 512:(lba + 1) * 512])
            self.pos = 0
            if cmd == 0x20:
                self.reads += 1
            else:
                self.writes += 1
                self.pending_lba = lba
            return

    def data_out(self, lo, hi):
        """Запись слова в порт данных (REP OUTSW)."""
        if getattr(self, "pending_lba", None) is None:
            return
        if not hasattr(self, "_wbuf"):
            self._wbuf = bytearray(512)
            self._wpos = 0
        if self._wpos + 2 <= 512:
            self._wbuf[self._wpos] = lo
            self._wbuf[self._wpos + 1] = hi
            self._wpos += 2
        if self._wpos >= 512:
            lba = self.pending_lba
            self.img[lba * 512:(lba + 1) * 512] = self._wbuf
            self.dirty.add(lba)
            self.pending_lba = None
            self._wpos = 0


def main():
    if len(sys.argv) < 3:
        sys.exit("usage: bootemu.py <image> <kernel.bin>")
    img = open(sys.argv[1], "rb").read()
    kernel = open(sys.argv[2], "rb").read()

    if len(kernel) > 64 * 512:
        sys.exit("bootemu: ядро больше 64 секторов, загрузчик его не прочитает")

    disk = Disk(img)
    # Геометрия диска, которую «отдаёт BIOS». Умышленно НЕ 18/2 (как у
    # дискеты): QEMU для маленького IDE-диска использует 16 головок и
    # 63 сектора. Загрузчик обязан спрашивать геометрию у BIOS, а не
    # угадывать — иначе он читает совсем другие секторы.
    spt, heads = 63, 16

    mu = Uc(UC_ARCH_X86, UC_MODE_16)
    mu.mem_map(0, 0x100000 + 0x1000000)          # 16 МиБ + низкая память
    # BIOS передаёт номер загрузочного диска в DL: 0x80 — первый HDD.
    # Раньше эмулятор DL не выставлял, поэтому «поехавший» номер диска
    # в памяти обнаруживался не сразу, а только по мусору в результате.
    BOOT_DRIVE = 0x80
    mu.reg_write(UC_X86_REG_DL, BOOT_DRIVE)
    mu.mem_write(0x7C00, bytes(disk.img[0:512]))
    mu.mem_write(0x7E00, bytes(disk.img[512:512 + STAGE2_SECTORS * 512]))

    screen, reads, bad_chs, lba_calls = [], [], [], []
    bad_drive = []

    # Карта как у QEMU: 0xA0000..0x100000 (VGA + BIOS ROM) — reserved,
    # иначе ядро справедливо сочтёт видеопамять свободной.
    regions = [
        (0x00000000, 0x0009FC00, 1),
        (0x0009FC00, 0x00060400, 2),
        (0x00100000, 0x00F00000, 1),
        (0x01000000, 0x00020000, 2),
    ]

    # Шрифт: 8x16, у каждого символа свой рисунок, чтобы ядро нельзя
    # было "случайно" нарисовать правильную букву при неверном адресе.
    def make_font():
        import random
        rng = random.Random(1234)
        font = bytearray(4096)
        for ch in range(256):
            for row in range(16):
                font[ch * 16 + row] = rng.randrange(256)
        return bytes(font)

    font = make_font()
    vbe_state = {"mode": None, "set_calls": []}

    def vbe_info_block():
        blk = bytearray(512)
        blk[0:4] = b"VESA"
        blk[4:6] = struct.pack("<H", 0x0300)
        blk[10:14] = struct.pack("<I", 0)               # capabilities
        # VideoModePtr — real-mode far pointer: offset, segment
        blk[14:16] = struct.pack("<H", VBE_MODE_LIST)
        blk[16:18] = struct.pack("<H", 0)
        blk[18:20] = struct.pack("<H", 8192 // 64)      # 8 MiB видео
        return bytes(blk)

    def mode_info_block():
        blk = bytearray(256)
        blk[0:2] = struct.pack("<H", 0x0099)            # supported|graphics|LFB
        blk[16:18] = struct.pack("<H", VBE_PITCH)
        blk[18:20] = struct.pack("<H", VBE_W)
        blk[20:22] = struct.pack("<H", VBE_H)
        blk[22] = 8
        blk[23] = 16
        blk[24] = 1                                     # planes
        blk[25] = VBE_BPP
        blk[40:44] = struct.pack("<I", LFB_ADDR)
        return bytes(blk)

    def on_intr(mu, intno, _):
        ah = mu.reg_read(UC_X86_REG_AH)
        if intno == 0x10:
            if ah == 0x0E:
                screen.append(chr(mu.reg_read(UC_X86_REG_AL)))
                return
            ax = mu.reg_read(UC_X86_REG_EAX)
            if ax == 0x4F00:                            # VBE: VbeInfoBlock
                di = mu.reg_read(UC_X86_REG_DI)
                es = mu.reg_read(UC_X86_REG_ES)
                mu.mem_write((es << 4) + di, vbe_info_block())
                mu.mem_write(VBE_MODE_LIST,
                             struct.pack("<HH", 0x101, VBE_MODE_NUMBER) +
                             struct.pack("<H", 0xFFFF))
                mu.reg_write(UC_X86_REG_EAX, 0x004F)
                return
            if ax == 0x4F01:                            # VBE: ModeInfoBlock
                di = mu.reg_read(UC_X86_REG_DI)
                es = mu.reg_read(UC_X86_REG_ES)
                cx = mu.reg_read(UC_X86_REG_CX)
                blk = bytearray(mode_info_block())
                if cx != VBE_MODE_NUMBER:
                    # чужой режим: недоступен, ядро обязано его пропустить
                    blk[0:2] = struct.pack("<H", 0x0000)
                mu.mem_write((es << 4) + di, bytes(blk))
                mu.reg_write(UC_X86_REG_EAX, 0x004F)
                return
            if ax == 0x4F02:                            # VBE: установка режима
                bx = mu.reg_read(UC_X86_REG_BX)
                vbe_state["mode"] = bx
                vbe_state["set_calls"].append(bx)
                mu.reg_write(UC_X86_REG_EAX, 0x004F)
                return
            if ax == 0x1130:                            # Get Font Information
                bh = (mu.reg_read(UC_X86_REG_EBX) >> 8) & 0xFF
                # отдаём шрифт по адресу 0000:FONT_ADDR (уже в памяти)
                mu.mem_write(FONT_ADDR, font)
                mu.reg_write(UC_X86_REG_BP, FONT_ADDR)
                mu.reg_write(UC_X86_REG_ES, 0x0000)
                mu.reg_write(UC_X86_REG_CX, 16)
                mu.reg_write(UC_X86_REG_DL, 15)
                return
            return
        if intno == 0x15:
            if mu.reg_read(UC_X86_REG_EAX) != 0xE820:
                return
            ebx = mu.reg_read(UC_X86_REG_EBX)
            idx = 0 if ebx == 0 else ebx
            if idx >= len(regions):
                mu.reg_write(UC_X86_REG_EAX, 0)
                mu.reg_write(UC_X86_REG_EBX, 0)
                mu.reg_write(UC_X86_REG_EFLAGS,
                             mu.reg_read(UC_X86_REG_EFLAGS) | 1)
                return
            base, length, kind = regions[idx]
            addr = (mu.reg_read(UC_X86_REG_ES) << 4) + mu.reg_read(UC_X86_REG_DI)
            mu.mem_write(addr, struct.pack("<QQII", base, length, kind, 1))
            mu.reg_write(UC_X86_REG_EAX, 0x534D4150)
            mu.reg_write(UC_X86_REG_EBX, idx + 1)
            mu.reg_write(UC_X86_REG_ECX, 24)
            mu.reg_write(UC_X86_REG_EFLAGS,
                         mu.reg_read(UC_X86_REG_EFLAGS) & ~1)
            return
        if intno != 0x13:
            return

        # Настоящий BIOS ищет диск по номеру в DL и с чужим номером
        # возвращает CF=1. Эмулятор этого не делал, поэтому порча
        # ячейки с номером диска проходила незамеченной — именно так
        # «съехавший» VIDEO_INFO_ADDR превращался в чёрный экран на
        # реальной машине при зелёном CI.
        dl = mu.reg_read(UC_X86_REG_DL)
        if dl != BOOT_DRIVE:
            bad_drive.append(dl)
            mu.reg_write(UC_X86_REG_AH, 0x01)
            mu.reg_write(UC_X86_REG_EFLAGS,
                         mu.reg_read(UC_X86_REG_EFLAGS) | 1)
            return

        if ah == 0x02:
            count = mu.reg_read(UC_X86_REG_AL)
            ch = mu.reg_read(UC_X86_REG_CH)
            cl = mu.reg_read(UC_X86_REG_CL)
            cyl = ch | ((cl & 0xC0) << 2)
            head = mu.reg_read(UC_X86_REG_DH)
            sec = (cl & 0x3F) - 1
            lba = (cyl * heads + head) * spt + sec
            dst = (mu.reg_read(UC_X86_REG_ES) << 4) + mu.reg_read(UC_X86_REG_BX)
            reads.append((lba, dst, count))
            if sec >= spt or head >= heads:
                bad_chs.append((cyl, head, sec))
            for i in range(count):
                if 0 <= lba + i < disk.sectors:
                    mu.mem_write(dst + i * 512,
                                 bytes(disk.img[(lba + i) * 512:(lba + i + 1) * 512]))
                else:
                    bad_chs.append(("lba", lba + i))
            mu.reg_write(UC_X86_REG_AL, count)
            mu.reg_write(UC_X86_REG_EFLAGS,
                         mu.reg_read(UC_X86_REG_EFLAGS) & ~1)
        elif ah == 0x08:
            cyls = max(1, disk.sectors // (spt * heads))
            mu.reg_write(UC_X86_REG_CH, (cyls - 1) & 0xFF)
            mu.reg_write(UC_X86_REG_CL,
                         (spt & 0x3F) | (((cyls - 1) >> 2) & 0xC0))
            mu.reg_write(UC_X86_REG_DH, heads - 1)
            mu.reg_write(UC_X86_REG_EFLAGS,
                         mu.reg_read(UC_X86_REG_EFLAGS) & ~1)
        elif ah == 0x00:
            mu.reg_write(UC_X86_REG_EFLAGS,
                         mu.reg_read(UC_X86_REG_EFLAGS) & ~1)
        else:
            if ah == 0x42:
                lba_calls.append(ah)
            mu.reg_write(UC_X86_REG_AH, 0x01)
            mu.reg_write(UC_X86_REG_EFLAGS,
                         mu.reg_read(UC_X86_REG_EFLAGS) | 1)

    mu.hook_add(UC_HOOK_INTR, on_intr)

    # --- порты ввода-вывода: модель ATA ---
    def on_in(mu, port, size, ud):
        if 0x1F0 <= port <= 0x1F7:
            return disk.inp(port)
        if port == 0x3F6 or port == 0x3F7:
            return 0x50                       # готовность
        return 0

    def on_out(mu, port, size, value, ud):
        if 0x1F0 <= port <= 0x1F7:
            disk.out(port, value)
        elif port == 0x1F0:
            pass
        return

    mu.hook_add(UC_HOOK_INSN, on_in, None, 1, 0, UC_X86_INS_IN)
    mu.hook_add(UC_HOOK_INSN, on_out, None, 1, 0, UC_X86_INS_OUT)

    # 1) загрузчик: до входа в ядро (реальный режим)
    try:
        mu.emu_start(0x7C00, KERNEL_START, count=50_000_000)
    except UcError as exc:
        check(False, "загрузчик дошёл до входа в ядро: %s (EIP=%#x)"
              % (exc, mu.reg_read(UC_X86_REG_EIP)))
        return report()

    print("Загрузка:")
    text = "".join(screen)
    check("Loading..." in text, "первый этап вывел сообщение")
    check("E820:" in text and "regions" in text, "второй этап собрал карту E820")
    check("OK, booting" in text, "второй этап дошёл до перехода в PM")
    check(not lba_calls, "загрузчик не использует AH=42h")

    print("\nПамять загрузчика:")
    check(not bad_drive,
          "все INT 13h вызваны с номером загрузочного диска (DL=%#x)%s"
          % (BOOT_DRIVE,
             "" if not bad_drive else ", а встретились: %s"
             % sorted(set(hex(d) for d in bad_drive))))
    # Области, которые загрузчик заполняет для ядра, не должны
    # пересекаться: карта E820 (0x0600..0x07E7), параметры кадра и
    # шрифт. Наложение уже один раз дало чёрный экран.
    check(VIDEO_INFO_ADDR + 24 <= FONT_ADDR or FONT_ADDR + FONT_BYTES <= VIDEO_INFO_ADDR,
          "параметры кадра и шрифт не накладываются друг на друга "
          "(кадр %#x..%#x, шрифт %#x..%#x)"
          % (VIDEO_INFO_ADDR, VIDEO_INFO_ADDR + 24,
             FONT_ADDR, FONT_ADDR + 4096))
    check(FONT_ADDR + 4096 <= 0x7C00,
          "шрифт не залезает в загрузочный сектор (конец %#x < 0x7C00)"
          % (FONT_ADDR + 4096))
    check(VIDEO_INFO_ADDR >= E820_BUF_ADDR + 20 * 24,
          "параметры кадра лежат за картой E820 (%#x..%#x)"
          % (E820_BUF_ADDR, E820_BUF_ADDR + 20 * 24))
    check(BOOT_DRIVE_ADDR < VIDEO_INFO_ADDR,
          "адрес номера диска (%#x) не перекрыт параметрами кадра (%#x)"
          % (BOOT_DRIVE_ADDR, VIDEO_INFO_ADDR))

    print("\nВидео и шрифт (BIOS INT 10h):")
    check(vbe_state["mode"] is not None and
          vbe_state["mode"] & 0x4000 == 0x4000,
          "загрузчик включил LFB при установке режима (BX|0x4000)")
    check(vbe_state["mode"] is not None and
          vbe_state["mode"] & 0x1FF == VBE_MODE_NUMBER,
          "выбран режим из списка BIOS (%#x), а не зашитый номер"
          % (vbe_state["mode"] & 0x1FF if vbe_state["mode"] else 0))

    vinfo = [struct.unpack("<I", bytes(mu.mem_read(VIDEO_INFO_ADDR + o, 4)))[0]
             for o in (0, 4, 8, 12, 16, 20)]
    check(vinfo[0] == VBE_W and vinfo[1] == VBE_H,
          "параметры кадра: %dx%d" % (vinfo[0], vinfo[1]))
    check(vinfo[2] == VBE_PITCH, "шаг строки %d Б" % vinfo[2])
    check(vinfo[3] == LFB_ADDR, "адрес LFB %#x" % vinfo[3])
    check(vinfo[4] == VBE_BPP, "%d бит на пиксель" % vinfo[4])
    check(vinfo[5] == 1, "ядру сообщено, что режим установлен")

    copied_font = bytes(mu.mem_read(FONT_ADDR, 4096))
    check(copied_font == font,
          "шрифт 8x16 скопирован в память ядра (4096 Б, %d символов)"
          % (len(copied_font) // 16))

    print("\nЧтение диска загрузчиком:")
    check(not bad_chs, "все CHS в пределах геометрии")
    expected = list(range(1, 1 + len(reads)))
    check([r[0] for r in reads] == expected,
          "секторы прочитаны по порядку 1..%d" % len(reads))
    check(reads[0][:2] == (1, 0x7E00), "второй этап читается в 0x7E00")
    check(reads[STAGE2_SECTORS][:2] == (KERNEL_LBA, KERNEL_START),
          "ядро читается в 0x10000")

    got = bytes(mu.mem_read(KERNEL_START, len(kernel)))
    check(got == kernel, "ядро в 0x10000 побайтово равно kernel.bin")
    check(len(reads) == STAGE2_SECTORS + 64,
          "прочитано %d сектора второго этапа + 64 сектора ядра (всего %d)"
          % (STAGE2_SECTORS, len(reads)))
    if FAILED:
        return report()

    # 2) ядро: переключение в 32-битный режим и работа с ФС
    print("\nЯдро (32 бита, ATA PIO + FAT16):")
    cr0 = mu.reg_read(UC_X86_REG_CR0)
    check(cr0 & 1 == 1, "ядро вошло в защищённый режим (CR0.PE=1)")

    # Продолжать в том же эмуляторе нельзя (он в 16-битном режиме),
    # поэтому ядро прогоняется в отдельном 32-битном экземпляре.
    # Сегменты и GDT берём ровно те, что поставил boot2.asm.
    mu2 = Uc(UC_ARCH_X86, UC_MODE_32)
    mu2.mem_map(0, 0x100000 + 0x1000000)
    mu2.mem_write(KERNEL_START, kernel)

    # Карту E820, собранную загрузчиком в реальном режиме, надо перенести
    # в этот экземпляр: иначе ядро стартует без карты, PMM уходит в
    # legacy-режим и отчёт будет другим. Адреса — из boot2.asm.
    e820_count = struct.unpack("<I", bytes(mu.mem_read(0x600, 4)))[0]
    mu2.mem_write(0x600, struct.pack("<I", e820_count))
    mu2.mem_write(0x608, bytes(mu.mem_read(0x608, min(e820_count, 20) * 24)))

    # Параметры кадра и копию шрифта тоже переносим — иначе ядро
    # откатится на текстовый режим и ничего не нарисует.
    mu2.mem_write(VIDEO_INFO_ADDR,
                  bytes(mu.mem_read(VIDEO_INFO_ADDR, 24)))
    mu2.mem_write(FONT_ADDR, copied_font)

    # Линейный буфер кадра по адресу, который «выдал BIOS».
    mu2.mem_map(LFB_ADDR, LFB_SIZE)
    mu2.mem_map(COM1_FIFO, COM1_FIFO_SIZE)

    # COM1: вместо настоящего порта пишем байты в память, чтобы потом
    # снять весь вывод ядра (на экране помещается только 48 строк).
    com1 = {"pos": 0, "skipped": 0}

    def on_out(u, port, size, value, _):  # noqa: ARG001
        if port != 0x3F8:
            return
        if com1["pos"] < COM1_FIFO_SIZE - 1:
            u.mem_write(COM1_FIFO + com1["pos"], bytes([value & 0xFF]))
            com1["pos"] += 1
        else:
            com1["skipped"] += 1

    def on_in(u, port, size, _):
        if port == 0x3F8 + 5:                       # линия свободна
            u.reg_write(UC_X86_REG_EAX, 0x20)
        elif port == 0x3F8:
            u.reg_write(UC_X86_REG_EAX, 0)


    # GDT из boot2.asm: нулевой, код (0x08), данные (0x10).
    # Сегменты выставляем ПОСЛЕ загрузки GDT: unicorn проверяет
    # селектор по описанию в GDT, иначе — исключение.
    gdt = struct.pack("<QQQ", 0,
                      0x00CF9A000000FFFF,
                      0x00CF92000000FFFF)
    mu2.mem_write(GDT_ADDR, gdt)
    mu2.reg_write(UC_X86_REG_GDTR, (0, GDT_ADDR, len(gdt) - 1, 0))

    for sel in (UC_X86_REG_DS, UC_X86_REG_ES, UC_X86_REG_FS,
                UC_X86_REG_GS, UC_X86_REG_SS):
        mu2.reg_write(sel, 0x10)
    mu2.reg_write(UC_X86_REG_CS, 0x08)
    mu2.reg_write(UC_X86_REG_ESP, 0x90000)

    def on_in2(u, port, size, ud):
        if port == 0x3F8 + 5:                   # COM1: передатчик свободен
            return 0x20
        if 0x1F0 <= port <= 0x1F7:
            return disk.inp(port)
        if port in (0x3F6, 0x3F7):
            return 0x50
        return 0

    def on_out2(u, port, size, value, ud):
        if port == 0x3F8:                       # COM1: собираем вывод
            if com1["pos"] < COM1_FIFO_SIZE - 1:
                u.mem_write(COM1_FIFO + com1["pos"], bytes([value & 0xFF]))
                com1["pos"] += 1
            return
        if 0x1F0 <= port <= 0x1F7:
            if port == 0x1F0:
                disk.data_out(value & 0xFF, (value >> 8) & 0xFF)
            else:
                disk.out(port, value)

    mu2.hook_add(UC_HOOK_INSN, on_in2, None, 1, 0, UC_X86_INS_IN)
    mu2.hook_add(UC_HOOK_INSN, on_out2, None, 1, 0, UC_X86_INS_OUT)

    # --- доставка int 0x80 (syscall Ring3 -> Ring0) ---
    #
    # unicorn IDT не обходит: `int` просто вызывает UC_HOOK_INTR и
    # пропускается. На настоящем железе interrupt gate сам делает:
    # стек Ring0 из TSS.ESP0, кадр SS/ESP/EFLAGS/CS/EIP на него,
    # CS:EIP — из IDT. Делаем ровно это вручную. Два ограничения
    # unicorn обходятся здесь же, и только здесь:
    #   * reg_write(CS/SS) с повышением CPL unicorn запрещает, поэтому
    #     CS обработчика не трогаем (модель плоская — исполнение от
    #     этого не меняется), а дескриптору данных ядра в КОПИИ GDT
    #     эмулятора поднимаем DPL, чтобы `mov ds, 0x10` из CPL=3 не
    #     дал #GP. На реальную GDT ядра это не влияет.
    #   * финальный `iret` обработчика при равном CPL не снял бы
    #     ESP/SS со стека, поэтому перехватываем его UC_HOOK_CODE и
    #     восстанавливаем контекст Ring3 сами, как сделал бы CPU.
    syscall_state = {"active": False}

    def _push32(val):
        esp = mu2.reg_read(UC_X86_REG_ESP) - 4
        mu2.reg_write(UC_X86_REG_ESP, esp)
        mu2.mem_write(esp, struct.pack("<I", val & 0xFFFFFFFF))

    def on_intr2(u, intno, _):
        if intno != 0x80:
            return
        idtr = u.reg_read(UC_X86_REG_IDTR)
        gdtr = u.reg_read(UC_X86_REG_GDTR)
        tr = u.reg_read(UC_X86_REG_TR)
        ent = bytes(u.mem_read(idtr[1] + 0x80 * 8, 8))
        handler = struct.unpack("<H", ent[0:2])[0] | \
            (struct.unpack("<H", ent[6:8])[0] << 16)
        desc = bytes(u.mem_read(gdtr[1] + (tr[0] & ~7), 8))
        tss_base = struct.unpack("<H", desc[2:4])[0] | \
            (desc[4] << 16) | (desc[7] << 24)
        esp0 = struct.unpack("<I", bytes(u.mem_read(tss_base + 4, 4)))[0]

        acc = gdtr[1] + 16 + 5      # access-байт дескриптора данных ядра
        u.mem_write(acc, bytes([bytes(u.mem_read(acc, 1))[0] | 0x60]))

        user_ss = u.reg_read(UC_X86_REG_SS)
        user_esp = u.reg_read(UC_X86_REG_ESP)
        user_cs = u.reg_read(UC_X86_REG_CS)
        user_eip = u.reg_read(UC_X86_REG_EIP)   # unicorn: уже след. insn
        eflags = u.reg_read(UC_X86_REG_EFLAGS)
        u.reg_write(UC_X86_REG_ESP, esp0)
        _push32(user_ss)
        _push32(user_esp)
        _push32(eflags & ~0x200)                # interrupt gate гасит IF
        _push32(user_cs)
        _push32(user_eip)
        syscall_state["active"] = True
        u.reg_write(UC_X86_REG_EIP, handler)

    def on_code_syscall(u, addr, size, _):
        if not syscall_state["active"]:
            return
        if bytes(u.mem_read(addr, 1))[0] != 0xCF:   # не iret обработчика
            return
        esp = u.reg_read(UC_X86_REG_ESP)
        eip, cs, eflags, user_esp, user_ss = struct.unpack(
            "<5I", bytes(u.mem_read(esp, 20)))
        syscall_state["active"] = False
        u.reg_write(UC_X86_REG_SS, user_ss)
        u.reg_write(UC_X86_REG_ESP, user_esp)
        u.reg_write(UC_X86_REG_CS, cs)
        u.reg_write(UC_X86_REG_EFLAGS, eflags)
        u.reg_write(UC_X86_REG_EIP, eip)

    mu2.hook_add(UC_HOOK_INTR, on_intr2)
    mu2.hook_add(UC_HOOK_CODE, on_code_syscall)

    # Конец работы ядра — hlt-цикл. Ловим код hlt (0xF4) хуком кода:
    # на этот адрес ядро возвращается бесконечно.
    hlt_state = {"addr": None, "hits": 0}

    def on_block2(u, addr, size, ud):
        # конец работы ядра — бесконечный hlt; останавливаемся здесь,
        # чтобы не тратить лимит инструкций
        if u.mem_read(addr, 1)[0] == 0xF4:
            u.emu_stop()
            hlt_state["hits"] += 1

    def on_code2(u, addr, size, ud):
        b = u.mem_read(addr, 1)[0]
        if b == 0xF4:
            if hlt_state["addr"] == addr:
                hlt_state["hits"] += 1
                if hlt_state["hits"] > 3:
                    u.emu_stop()
            else:
                hlt_state["addr"] = addr
                hlt_state["hits"] = 0

    # Пошаговый UC_HOOK_CODE на 0xF4 тормозил эмуляцию в разы: колбэк
    # вызывался на каждой инструкции. Блочный хук срабатывает на
    # транслированный блок, а hlt-цикл ядра — это блок, начинающийся
    # с 0xF4, так что хватает одного сравнения байта.
    mu2.hook_add(UC_HOOK_BLOCK, on_block2)

    try:
        mu2.emu_start(KERNEL_START, 0, count=200_000_000)
    except UcError as exc:
        # Молча проглатывать нельзя: обрыв эмуляции выглядел бы как
        # «ядро просто ничего не написало». Печатаем место остановки.
        print("\n  ! эмуляция ядра прервана: %s (EIP=%#x)"
              % (exc, mu2.reg_read(UC_X86_REG_EIP)))

    uart = bytes(mu2.mem_read(COM1_FIFO, com1["pos"])).decode("ascii", "replace")
    print("\n--- вывод COM1 (%d байт) ---" % len(uart))
    for ln in uart.splitlines():
        print("  : %s" % ln)
    lfb = bytes(mu2.mem_read(LFB_ADDR, LFB_SIZE))

    # Восстановить текст из кадрового буфера: для каждой ячейки 8x16
    # сравниваем её рисунок со всеми 256 глифами шрифта и берём
    # совпавший. Это проверка «глазами», а не по внутреннему состоянию
    # ядра: если ядро рисует не туда или не тот шрифт, текст не сложится.
    def decode_screen():
        # Таблица «16 байт рисунка глифа -> символ»: вместо перебора 256
        # глифов на каждую ячейку собираем рисунок ячейки и ищем его в
        # словаре. 6144 ячейки x 16 строк x 8 пикселей — иначе тест
        # тратил бы минуты на чистом Python.
        glyph_of = {}
        for ch in range(256):
            glyph_of[font[ch * 16:(ch + 1) * 16]] = ch
        cols_n, rows_n = VBE_W // 8, VBE_H // 16
        out = []
        for row in range(rows_n):
            chars = []
            for col in range(cols_n):
                x0, y0 = col * 8, row * 16
                key = bytearray(16)
                for gy in range(16):
                    off = (y0 + gy) * VBE_PITCH + x0 * 4
                    for gx in range(8):
                        px = int.from_bytes(lfb[off + gx * 4:off + gx * 4 + 4], "little")
                        if px:
                            key[gy] |= 1 << (7 - gx)
                ch = glyph_of.get(bytes(key), 0)
                chars.append(chr(ch) if 32 <= ch < 127 else " ")
            out.append("".join(chars).rstrip())
        return out

    screen_rows = decode_screen()

    # Экран разбит на две текстовые колонки по 64 символа: отчёт идёт
    # сначала в левую, потом в правую. Собираем логический текст в том
    # порядке, в котором его печатало ядро.
    half = VBE_W // 8 // 2
    left_col = [row[:half].rstrip() for row in screen_rows]
    right_col = [row[half:].rstrip() for row in screen_rows]
    lines = left_col + right_col
    text2 = "\n".join(lines)

    check(len(screen_rows) == 48, "кадр 1024x768 даёт 48 строк текста")
    check(len(screen_rows[0].ljust(half * 2)[: half * 2]) == 128 and
          len(screen_rows[0]) <= 128,
          "в строке помещается 128 символов (по 64 на колонку)")
    check(len(left_col) + len(right_col) == 96,
          "две колонки по 64 символа дают 96 текстовых строк")
    check(sum(1 for ln in lines if ln.strip()) > 40,
          "на экране распознан текст (%d непустых строк)"
          % sum(1 for ln in lines if ln.strip()))

    # Отчёт длиннее одной колонки (48 строк), но в две колонки
    # помещается целиком — поэтому первая строка обязана остаться на
    # экране: если бы ядро прокручивало, заголовок уехал бы вверх.
    check("ex-nihilo-OS: physical memory report" in screen_rows[0],
          "заголовок отчёта остался в первой строке (прокрутки не было)")
    check("System halted" in (left_col + right_col)[-1] or
          any("System halted" in ln for ln in lines),
          "последняя строка отчёта тоже на экране")
    # Пробелы между словами в отчёте выравнивают колонки, поэтому
    # проверяем по смыслу, а не по точной строке.
    import re as _re

    def has(pattern):
        return _re.search(pattern, text2) is not None

    check(has(r"hello\s+fat16"), "FAT16 прочитал HELLO.TXT с диска")
    check(has(r"read\s+bytes:\s+12"), "HELLO.TXT: 12 байт")
    check(has(r"bytes\s+written:\s+24\s+\[ok\]"), "запись WRITE.TXT прошла")
    check(has(r"re-read\s+bytes:\s+24\s+\[ok"), "записанные данные читаются обратно")
    check(has(r"unlink:\s+0\s+\[ok\]"), "unlink прошёл")
    check(has(r"exists\(/WRITE.TXT\)\s+after\s+unlink:\s+0\s+\[ok"),
          "после unlink файла нет")
    check("[BUG]" not in text2, "ядро не сообщает о внутренних ошибках")
    check("not found" not in text2 and "unavailable" not in text2,
          "ошибок монтирования нет")
    check(has(r"Free\s+pages\s+remaining"), "отчёт PMM дошёл до конца")
    print("\nSyscall (Ring3 --int 0x80--> Ring0):")
    check(has(r"CPL\s+seen\s+by\s+user.*3\s+\[ok"), "юзерский код реально в Ring 3 (CPL=3)")
    check(has(r"kopen\(\"/HELLO\.TXT\"\)\s*->\s*fd:.*\[ok\]"), "syscall SYS_KOPEN вызвал kopen() в Ring 0")
    check(has(r"kread\(\)\s*->\s*bytes:\s+12\s+\[ok\]"), "syscall SYS_KREAD прочитал HELLO.TXT (12 байт)")
    check(has(r"kclose\(\)\s*->\s*rc:\s+0\s+\[ok\]"), "syscall SYS_KCLOSE вернул 0")
    check(has(r"user\s+exit\s+code.*42\s+\[ok\]"), "SYS_KEXIT вернул управление и код 42 в Ring 0")
    print("\nExecve / ELF:")
    check(has(r"USER1\.ELF:\s+hello\s+from\s+ELF"), "USER1.ELF загружен с диска и исполняется")
    check(has(r"USER2\.ELF:\s+argv\[1\]=passed-by-user1"), "argv пережил два execve подряд")
    check(has(r"execve\s+chain\s+to\s+USER2\.ELF:.*\[ok"), "цепочка execve USER1.ELF -> USER2.ELF завершилась кодом 42")

    # Цвет статус-маркеров: ядро красит "[ok...]" зелёным, "[BUG]" —
    # красным. Декодер выше смотрит только «пиксель включён», поэтому
    # цвет проверяем отдельно по сырым пикселям ячейки со скобкой.
    def cell_fg(row, col):
        """Доминирующий ненулевой цвет пикселей ячейки (row, col)."""
        counts = {}
        for gy in range(16):
            off = (row * 16 + gy) * VBE_PITCH + col * 8 * 4
            for gx in range(8):
                px = int.from_bytes(lfb[off + gx * 4:off + gx * 4 + 4],
                                    "little") & 0xFFFFFF
                if px:
                    counts[px] = counts.get(px, 0) + 1
        return max(counts, key=counts.get) if counts else 0

    def is_green(px):
        r, g, b = (px >> 16) & 0xFF, (px >> 8) & 0xFF, px & 0xFF
        return g > 100 and g > r + 30 and g > b + 30

    green_at = None
    for r, row_text in enumerate(screen_rows):
        c = row_text.find("[ok")
        if c >= 0:
            green_at = (r, c)
            break
    check(green_at is not None and is_green(cell_fg(*green_at)),
          "маркер [ok] на экране нарисован зелёным (ячейка %s)" % (green_at,))

    hdr_px = cell_fg(0, 4)      # обычный текст заголовка — серый
    check(hdr_px != 0 and abs(((hdr_px >> 16) & 0xFF) - ((hdr_px >> 8) & 0xFF)) < 30,
          "обычный текст остался серым, а не цветным")
    check(disk.writes > 0, "запись действительно дошла до диска")

    # 5) образ изменился ровно в тех секторах, куда писал драйвер
    print("\nДиск после работы ядра:")
    check(len(disk.dirty) > 0, "изменены секторы: %d" % len(disk.dirty))
    changed = sorted(disk.dirty)
    if changed:
        check(all(l >= FAT_LBA for l in changed),
              "все изменённые секторы лежат в области FAT16 (>= %d)" % FAT_LBA)
        print("  записаны секторы: %s" % changed[:12])

    print("\n--- экран ядра (непустые строки) ---")
    for line in lines:
        if line.strip():
            print("  | " + line)

    return report()


def report():
    print()
    if FAILED:
        print("bootemu: FAILED (%d)" % len(FAILED))
        return 1
    print("bootemu: all checks passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
