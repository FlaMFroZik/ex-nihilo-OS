OUTPUT_FORMAT("binary")
ENTRY(kmain)

SECTIONS
{
    . = 0x1000;

    .text : SUBALIGN(4) {
        *(.text)
        *(.rodata)
    }

    .data : SUBALIGN(4) {
        *(.data)
        *(.bss)
        *(COMMON)
    }

    /DISCARD/ : {
        *(.eh_frame)
        *(.comment)
    }
}
