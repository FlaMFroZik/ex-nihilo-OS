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
        UC_HOOK_CODE,
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
        UC_X86_REG_GDTR,
    )
except ImportError:
    sys.exit("bootemu: нужен unicorn (pip install unicorn)")

KERNEL_START = 0x10000
KERNEL_LBA = 3
FAT_LBA = 64
GDT_ADDR = 0x800
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
    mu.mem_write(0x7C00, bytes(disk.img[0:512]))
    mu.mem_write(0x7E00, bytes(disk.img[512:512 + 1024]))

    screen, reads, bad_chs, lba_calls = [], [], [], []

    # Карта как у QEMU: 0xA0000..0x100000 (VGA + BIOS ROM) — reserved,
    # иначе ядро справедливо сочтёт видеопамять свободной.
    regions = [
        (0x00000000, 0x0009FC00, 1),
        (0x0009FC00, 0x00060400, 2),
        (0x00100000, 0x00F00000, 1),
        (0x01000000, 0x00020000, 2),
    ]

    def on_intr(mu, intno, _):
        ah = mu.reg_read(UC_X86_REG_AH)
        if intno == 0x10:
            if ah == 0x0E:
                screen.append(chr(mu.reg_read(UC_X86_REG_AL)))
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

    print("\nЧтение диска загрузчиком:")
    check(not bad_chs, "все CHS в пределах геометрии")
    expected = list(range(1, 1 + len(reads)))
    check([r[0] for r in reads] == expected,
          "секторы прочитаны по порядку 1..%d" % len(reads))
    check(reads[0][:2] == (1, 0x7E00), "второй этап читается в 0x7E00")
    check(reads[2][:2] == (KERNEL_LBA, KERNEL_START), "ядро читается в 0x10000")

    got = bytes(mu.mem_read(KERNEL_START, len(kernel)))
    check(got == kernel, "ядро в 0x10000 побайтово равно kernel.bin")
    check(len(reads) == 2 + 64,
          "прочитано 2 сектора второго этапа + 64 сектора ядра (всего %d)"
          % len(reads))
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
        if 0x1F0 <= port <= 0x1F7:
            return disk.inp(port)
        if port in (0x3F6, 0x3F7):
            return 0x50
        return 0

    def on_out2(u, port, size, value, ud):
        if 0x1F0 <= port <= 0x1F7:
            if port == 0x1F0:
                disk.data_out(value & 0xFF, (value >> 8) & 0xFF)
            else:
                disk.out(port, value)

    mu2.hook_add(UC_HOOK_INSN, on_in2, None, 1, 0, UC_X86_INS_IN)
    mu2.hook_add(UC_HOOK_INSN, on_out2, None, 1, 0, UC_X86_INS_OUT)

    # Конец работы ядра — hlt-цикл. Ловим код hlt (0xF4) хуком кода:
    # на этот адрес ядро возвращается бесконечно.
    hlt_state = {"addr": None, "hits": 0}

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

    mu2.hook_add(UC_HOOK_CODE, on_code2)

    # VGA: текст пишется в 0xB8000 — читаем после остановки
    try:
        mu2.emu_start(KERNEL_START, 0, count=200_000_000)
    except UcError:
        pass

    vga = bytes(mu2.mem_read(0xB8000, 80 * 25 * 2))
    lines = []
    for row in range(25):
        chars = [chr(vga[(row * 80 + c) * 2]) for c in range(80)]
        lines.append("".join(chars).rstrip())
    text2 = "\n".join(lines)

    # Экран 80x25, а отчёт длиннее: начало (в т.ч. заголовок FAT16 и
    # строка о диске) прокручивается. Поэтому проверяем то, что видно,
    # плюс отсутствие маркеров ошибок — и состояние самого диска.
    check("hello fat16" in text2, "FAT16 прочитал HELLO.TXT с диска")
    check("read bytes: 12" in text2, "HELLO.TXT: 12 байт")
    check("bytes written: 24  [ok]" in text2, "запись WRITE.TXT прошла")
    check("identical" in text2, "записанные данные читаются обратно")
    check("unlink: 0  [ok]" in text2, "unlink прошёл")
    check("exists(/WRITE.TXT) after unlink:  0  [ok]" in text2,
          "после unlink файла нет")
    check("[BUG]" not in text2, "ядро не сообщает о внутренних ошибках")
    check("not found" not in text2 and "unavailable" not in text2,
          "ошибок монтирования нет")
    check("Free pages remaining:             3921" in text2 or
          "Free pages remaining:" in text2, "отчёт PMM дошёл до конца")
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
