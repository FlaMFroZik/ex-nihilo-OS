; tests/fat_start.asm — точка входа для host-теста FAT16
;
; Тест линкуется с настоящим fs/fat16.c и запускается как обычная
; 32-битная Linux-программа. Аргументы командной строки разворачиваются
; из стека по соглашению cdecl (_start(argc, argv) в Linux i386).

[BITS 32]

global _start
extern test_main

section .text.start progbits alloc exec nowrite align=16
_start:
    xor ebp, ebp
    cld
    and esp, 0xFFFFFFF0

    mov eax, [esp]              ; argc
    lea ebx, [esp + 4]          ; argv
    push ebx
    push eax
    call test_main

    add esp, 8
    mov ebx, eax                ; код возврата
    mov eax, 1                  ; SYS_exit
    int 0x80
.hang:
    hlt
    jmp .hang

section .note.GNU-stack noalloc noexec nowrite progbits
