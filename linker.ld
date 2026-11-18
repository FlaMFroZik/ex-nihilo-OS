/* linker.ld — раскладка ядра ex-nihilo-OS
 *
 * Ядро загружается загрузчиком по физическому адресу 0x10000,
 * поэтому .text начинается именно там, а самой первой идёт
 * секция .text.start с точкой входа kernel_start из kernel.asm
 * (загрузчик делает jmp 0x10000, а не вызов символа).
 */
OUTPUT_FORMAT("binary")
ENTRY(kernel_start)

SECTIONS
{
    . = 0x10000;

    .text : SUBALIGN(4) {
        KEEP(*(.text.start))        /* точка входа — всегда первая */
        *(.text)
        *(.text.*)
        *(.rodata)
        *(.rodata.*)
    }

    .data : SUBALIGN(4) {
        *(.data)
        *(.data.*)
    }

    /* Регион пользовательского кода внутри ядра (kernel/user.c):
     * отдельные страницы. paging перепомечает их User, остальные
     * страницы ядра остаются Supervisor — поэтому user-код и его
     * данные живут в своих секциях .usertext/.userdata/.userbss
     * (имена без префикса .text., чтобы их не проглотили вайлдкарды
     * выше).
     * ВАЖНО: регион стоит ДО .bss не случайно. .userdata —
     * инициализированная (PROGBITS), а плоский binary заполняет
     * нулями всё от конца предыдущей PROGBITS-секции до неё: после
     * 128-КиБ bitmap из .bss образ раздулся бы на весь её размер. */
    . = ALIGN(4096);
    user_region_start = .;
    .usertext : { *(.usertext) *(.usertext.*) }
    .userdata : { *(.userdata) *(.userdata.*) }
    .userbss : { *(.userbss) *(.userbss.*) }
    . = ALIGN(4096);
    user_region_end = .;

    /* .bss не попадает в файл (образ — плоский binary), но адреса
     * всё равно резервируются: pmm_init() обнуляет bitmap сам. */
    .bss : SUBALIGN(4) {
        *(.bss)
        *(.bss.*)
        *(COMMON)
    }

    /* Конец образа ядра: всё до него PMM считает занятым. */
    . = ALIGN(4096);
    kernel_end = .;

    /DISCARD/ : {
        *(.eh_frame)
        *(.eh_frame_hdr)
        *(.comment)
        *(.note*)
    }
}
