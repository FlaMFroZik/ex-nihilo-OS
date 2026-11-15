; tests/start.asm — точка входа для host-теста PMM
;
; Тест — обычный 32-битный Linux-исполняемый файл, который линкуется
; с настоящими hal/pmm.asm и hal/spinlock.asm. Так логика PMM
; проверяется на реальном коде, а не на его перепечатке.
;
; Здесь же задаётся символ kernel_end, который в настоящем ядре
; приходит из linker.ld. Берём 0x108000: это имитирует ядро,
; загруженное по 0x10000 и занимающее 32 КиБ (как KERNEL_SECTORS
; в boot.asm), то есть страницы 0x10..0x107.

[BITS 32]

global kernel_end
kernel_end equ 0x108000

global _start
global test_lock              ; переменная лока для теста спинлока
extern test_main

section .bss
    alignb 16
test_lock: resd 1

; Память под карту E820 и ячейку BIOS: в user space адреса 0x600 и
; 0x413 не отображены, поэтому при сборке теста hal/pmm.asm получает
; их через -DE820_COUNT_ADDR=... / -DE820_BUF_ADDR=... (см. Makefile).
global test_e820_count
global test_e820_buf
global test_bios_mem_kb
    alignb 16
test_e820_count: resd 1
test_e820_buf:   resb 20 * 24
test_bios_mem_kb: resw 1

section .text.start progbits alloc exec nowrite align=16
_start:
    xor ebp, ebp
    cld
    and esp, 0xFFFFFFF0
    call test_main

    ; exit_group не нужен: сисколл 1 (exit) завершает единственный поток
    mov ebx, eax                ; код возврата
    mov eax, 1                  ; SYS_exit
    int 0x80
.hang:
    hlt
    jmp .hang

section .note.GNU-stack noalloc noexec nowrite progbits
