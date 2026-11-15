#!/usr/bin/env python3
"""bootemu.py — эмуляционная проверка загрузчика ex-nihilo-OS.

Загрузчик нельзя проверить host-тестами: он целиком состоит из
BIOS-прерываний. Этот скрипт запускает boot.asm → boot2.asm на
эмуляторе (unicorn) и подменяет ровно те прерывания, что нужны:

  INT 10h/AH=0Eh — вывод символа: собираем текст экрана;
  INT 13h/AH=02h — чтение сектора: отдаём данные из os.img,
                   но ТОЛЬКО если переданные CHS соответствуют
                   запрошенному LBA (иначе тест падает);
  INT 13h/AH=42h — «не реализовано» (так ведёт себя SeaBIOS
                   с флоппи: см. комментарий в boot.asm);
  INT 15h/AX=E820h — карта памяти, как её отдаёт QEMU.

Проверяется:
  1. загрузчик не пользуется LBA-чтением (AH=42h) на флоппи;
  2. LBA→CHS считается верно для всех 66 секторов образа;
  3. цель чтения каждый раз сдвигается на 512 байт;
  4. ядро оказывается в памяти по 0x10000 побайтово равным
     build/kernel.bin;
  5. на экран выводится ожидаемый текст.

Запуск: python3 tools/bootemu.py build/os.img build/kernel.bin
Требует: pip install unicorn
"""
import struct
import sys

try:
    from unicorn import Uc, UcError, UC_ARCH_X86, UC_MODE_16, UC_HOOK_INTR
    from unicorn.x86_const import (
        UC_X86_REG_AH, UC_X86_REG_AL, UC_X86_REG_BX, UC_X86_REG_CH,
        UC_X86_REG_CL, UC_X86_REG_DH, UC_X86_REG_DI, UC_X86_REG_DL,
        UC_X86_REG_EAX, UC_X86_REG_EBX, UC_X86_REG_ECX, UC_X86_REG_EFLAGS,
        UC_X86_REG_EIP, UC_X86_REG_ES,
    )
except ImportError:
    sys.exit("bootemu: нужен unicorn (pip install unicorn)")

SPT, HEADS = 18, 2           # геометрия дискеты 1.44 МБ
TRACK = SPT * HEADS
KERNEL_START = 0x10000
KERNEL_LBA = 3               # 0 — boot, 1..2 — boot2, 3.. — ядро
FAILED = []


def check(ok, msg):
    print(("  [ok]   " if ok else "  [FAIL] ") + msg)
    if not ok:
        FAILED.append(msg)


def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__.strip().splitlines()[-2])
    img = open(sys.argv[1], "rb").read()
    kernel = open(sys.argv[2], "rb").read()
    sectors = [img[i * 512:(i + 1) * 512] for i in range(len(img) // 512)]

    mu = Uc(UC_ARCH_X86, UC_MODE_16)
    mu.mem_map(0, 0x120000)
    mu.mem_write(0x7C00, sectors[0])

    screen, reads, bad_chs, lba_calls = [], [], [], []

    # E820-карта как у QEMU: 6 записей
    regions = [
        (0x00000000, 0x0009FC00, 1),
        (0x0009FC00, 0x00000400, 2),
        (0x000F0000, 0x00010000, 2),
        (0x00100000, 0x07EE0000, 1),
        (0x07FE0000, 0x00020000, 2),
        (0xFFFC0000, 0x00040000, 2),
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
            lba = (cyl * HEADS + head) * SPT + sec
            dst = (mu.reg_read(UC_X86_REG_ES) << 4) + mu.reg_read(UC_X86_REG_BX)
            reads.append((lba, dst, count))
            if cyl > 79 or head >= HEADS or sec >= SPT:
                bad_chs.append((cyl, head, sec))
            for i in range(count):
                data = sectors[lba + i] if lba + i < len(sectors) else bytes(512)
                mu.mem_write(dst + i * 512, data)
            mu.reg_write(UC_X86_REG_AL, count)
            mu.reg_write(UC_X86_REG_EFLAGS,
                         mu.reg_read(UC_X86_REG_EFLAGS) & ~1)
        elif ah == 0x00:                       # сброс — успех
            mu.reg_write(UC_X86_REG_EFLAGS,
                         mu.reg_read(UC_X86_REG_EFLAGS) & ~1)
        elif ah == 0x08:                       # геометрия дискеты
            mu.reg_write(UC_X86_REG_CH, 79)
            mu.reg_write(UC_X86_REG_CL, SPT)
            mu.reg_write(UC_X86_REG_DH, HEADS - 1)
            mu.reg_write(UC_X86_REG_EFLAGS,
                         mu.reg_read(UC_X86_REG_EFLAGS) & ~1)
        else:
            if ah == 0x42:
                lba_calls.append(ah)           # так ведёт себя SeaBIOS
            mu.reg_write(UC_X86_REG_AH, 0x01)
            mu.reg_write(UC_X86_REG_EFLAGS,
                         mu.reg_read(UC_X86_REG_EFLAGS) | 1)

    mu.hook_add(UC_HOOK_INTR, on_intr)

    try:
        mu.emu_start(0x7C00, KERNEL_START, count=50_000_000)
    except UcError as exc:
        check(False, "эмуляция дошла до входа в ядро: %s (EIP=%#x)"
              % (exc, mu.reg_read(UC_X86_REG_EIP)))
        return report()

    print("Загрузка:")
    text = "".join(screen)
    check("Loading..." in text, "первый этап вывел сообщение")
    check("E820: 6 regions" in text, "второй этап собрал карту памяти E820")
    check("OK, booting" in text, "второй этап дошёл до перехода в PM")
    check(not lba_calls,
          "загрузчик не использует AH=42h (для флоппи это отказ BIOS)")

    print("\nЧтение диска:")
    check(not bad_chs, "все CHS в пределах геометрии 80/2/18")
    expected = list(range(1, 1 + len(reads)))
    check([r[0] for r in reads] == expected,
          "секторы прочитаны по порядку 1..%d" % len(reads))
    check(len(reads) == 2 + 64,
          "прочитано 2 сектора второго этапа + 64 сектора ядра")
    check(len(reads) > 0 and reads[0][:2] == (1, 0x7E00),
          "второй этап читается в 0x7E00")
    check(len(reads) > 2 and reads[2][:2] == (KERNEL_LBA, KERNEL_START),
          "ядро читается в 0x10000")

    # цель растёт на 512 в пределах каждой фазы
    phase1 = [r[1] for r in reads[:2]]
    phase2 = [r[1] for r in reads[2:]]
    check(phase1 == [0x7E00, 0x8000], "цель второго этапа растёт на 512")
    check(len(phase2) > 1 and
          all(phase2[i + 1] - phase2[i] == 512 for i in range(len(phase2) - 1)),
          "цель ядра растёт ровно на 512 байт")

    print("\nПамять:")
    try:
        got = bytes(mu.mem_read(KERNEL_START, len(kernel)))
    except UcError:
        got = b""
    check(got == kernel,
          "ядро в 0x10000 побайтово равно build/kernel.bin")

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
