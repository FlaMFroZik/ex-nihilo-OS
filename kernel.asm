[BITS 32]

kernel_start:
    ; Настройка сегментов
    mov ax, 0x10      ; Селектор данных из GDT загрузчика
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax

    ; Инициализация стека
    mov esp, 0x8FFFC

    ; Вызов C-функции
    call kmain

    cli
    hlt
    jmp $

extern kmain
