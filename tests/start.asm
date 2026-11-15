; tests/start.asm — точка входа для host-теста PMM
;
; Тест — обычный 32-битный Linux-исполняемый файл, который линкуется
; с настоящими hal/pmm.asm и hal/spinlock.asm. Так логика PMM
; проверяется на реальном коде, а не на его перепечатке.
;
; Здесь же задаётся символ kernel_end, который в настоящем ядре
; приходит из linker.ld. Берём 0x2000 — значит страницы 0 и 1
; (8 КиБ) считаются занятыми «ядром».

[BITS 32]

global kernel_end
kernel_end equ 0x2000

global _start
global test_lock              ; переменная лока для теста спинлока
extern test_main

section .bss
    alignb 4
test_lock: resd 1

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

section .note.GNU-stack noalloc noexec nowrite progbits
