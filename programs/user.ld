/* user.ld — раскладка пользовательских ELF (programs/).
 *
 * Бинарники грузятся kexecve() по виртуальным адресам как есть
 * (плоская модель, vaddr == физический адрес), поэтому адрес
 * выбираем в свободной памяти: 0x400000 (4 МиБ) — выше ядра,
 * его стека и страниц PMM, ниже конца типовой RAM из E820.
 * Формат именно ELF (OUTPUT_FORMAT не задаём): файл на диске
 * должен остаться ELF, его разбирает загрузчик в ядре. */
ENTRY(_start)

SECTIONS
{
    . = 0x400000;

    .text : {
        KEEP(*(.text.start))
        *(.text)
        *(.text.*)
    }
    .rodata : {
        *(.rodata)
        *(.rodata.*)
    }
    .data : {
        *(.data)
        *(.data.*)
    }
    .bss : {
        *(.bss)
        *(.bss.*)
        *(COMMON)
    }

    /DISCARD/ : {
        *(.eh_frame)
        *(.eh_frame_hdr)
        *(.comment)
        *(.note*)
    }
}
