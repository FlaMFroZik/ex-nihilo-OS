[BITS 32]

; ============================================================
; kernel.asm — 32-битная точка входа ядра
;
; Ядро грузится загрузчиком по физическому адресу 0x10000
; (см. boot.asm), поэтому здесь уже действует защищённый режим
; с плоской моделью памяти из GDT загрузчика.
;
; Стек ставим в 0x90000: он растёт вниз, в 0x80000..0x90000 —
; это ниже EBDA (0x9FC00) и выше загруженного ядра (0x10000),
; поэтому стек не затирает ни код ядра, ни таблицы BIOS.
; Тот же диапазон резервирует PMM (см. hal/pmm.asm).
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

    ; Инициализация стека.
    ; i386 SysV ABI: в точке инструкции call ESP должен быть кратен 16.
    ; call положит адрес возврата и сдвинет стек на 4 — тогда на входе
    ; в kmain исполняется (ESP + 4) % 16 == 0, чего и ждёт gcc.
    ; STACK_TOP = 0x90000 кратен 16, and — страховка на будущее.
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
