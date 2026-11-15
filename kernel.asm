[BITS 32]

; ============================================================
; kernel.asm — 32-битная точка входа ядра
;
; Ядро грузится загрузчиком по физическому адресу 0x10000
; (см. boot.asm), поэтому здесь уже действует защищённый режим
; с плоской моделью памяти из GDT загрузчика.
;
; Стек ставим в 0x90000 — это ниже 0x9FC00 (начало EBDA)
; и ниже загруженного ядра (0x10000..0x18000), поэтому стек
; не затирает ни код ядра, ни таблицы BIOS.
; ============================================================

STACK_TOP equ 0x90000

global kernel_start
extern kmain

section .text.start progbits alloc exec nowrite align=16
kernel_start:
    cli

    ; Настройка сегментов
    mov ax, 0x10            ; селектор данных из GDT загрузчика
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax

    ; Инициализация стека
    mov esp, STACK_TOP
    and esp, 0xFFFFFFF0
    mov ebp, esp

    cld                     ; ABI требует DF=0 при вызове C-кода

    ; Вызов C-функции
    call kmain

    ; kmain не возвращается, но на всякий случай
.hang:
    cli
    hlt
    jmp .hang

section .note.GNU-stack noalloc noexec nowrite progbits
