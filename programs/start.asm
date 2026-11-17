[BITS 32]

; ================================================================
; start.asm — _start для пользовательских ELF (programs/*.c).
;
; Ядро (kexecve) кладёт на стек классический кадр:
;   [ESP]   argc
;   [ESP+4] argv[0], argv[1], ..., NULL, envp[0], ..., NULL
; Достаём argc/argv, зовём prog_main(argc, argv), а её результат
; отдаём ядру через SYS_KEXIT (int 0x80, номер 0, код — в EBX).
; ================================================================

global _start
extern prog_main

section .text.start progbits alloc exec nowrite align=16
_start:
    mov eax, [esp]              ; argc
    lea ebx, [esp + 4]          ; argv
    push ebx
    push eax
    call prog_main

    mov ebx, eax                ; код завершения
    xor eax, eax                ; SYS_KEXIT
    int 0x80
.hang:
    jmp .hang

section .note.GNU-stack noalloc noexec nowrite progbits
