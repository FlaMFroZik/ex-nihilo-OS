[BITS 16]
[ORG 0x7C00]

start:
    ; Инициализация сегментов
    cli
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov sp, 0x7C00          ; стек загрузчика
    sti

    ; Сохраняем номер загрузочного диска
    mov [boot_drive], dl

    ; Вывод сообщения
    mov si, msg_loading
    call print_string

    ; Загрузка ядра: 1 сектор, сектор 2, в 0x1000
    mov ah, 0x02
    mov al, 1
    mov ch, 0               ; цилиндр
    mov cl, 2               ; сектор (1-й — загрузчик)
    mov dh, 0               ; головка
    mov dl, [boot_drive]
    mov bx, 0x1000          ; ES:BX = 0x0000:0x1000
    int 0x13
    jc disk_error

    ; Проверка: можно убрать эту проверку или заменить на свою сигнатуру
    ; cmp word [0x1000], 0xAA55
    ; jne kernel_error

    mov si, msg_ok
    call print_string

    ; Переход в защищённый режим
    cli
    lgdt [gdt_descriptor]

    mov eax, cr0
    or eax, 1               ; PE = 1
    mov cr0, eax

    jmp 0x08:protected_mode

; ============================================================
; 16-битный вывод строки
; ============================================================
print_string:
    pusha
    mov ah, 0x0E
.next_char:
    lodsb
    test al, al
    jz .done
    int 0x10
    jmp .next_char
.done:
    popa
    ret

; ============================================================
; 32-битный защищённый режим
; ============================================================
[BITS 32]
protected_mode:
    ; Настройка сегментных регистров (селектор данных 0x10)
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax
    mov esp, 0x8FFFC        ; стек (растёт вниз)

    ; Переход к ядру по адресу 0x1000
    jmp 0x1000

; ============================================================
; Данные (16 бит)
; ============================================================
[BITS 16]
boot_drive:  db 0

msg_loading: db "Loading kernel...", 0x0D, 0x0A, 0
msg_ok:      db "OK! Entering protected mode...", 0x0D, 0x0A, 0
msg_disk_err: db "Disk read error!", 0x0D, 0x0A, 0
msg_kern_err: db "Kernel not found!", 0x0D, 0x0A, 0

; ============================================================
; GDT
; ============================================================
align 8                 ; выравнивание GDT
gdt_start:
    dq 0                ; нулевой дескриптор

gdt_code:               ; селектор 0x08
    dw 0xFFFF           ; лимит (низ)
    dw 0x0000           ; база (низ)
    db 0x00             ; база (сред)
    db 10011010b        ; доступ: present, ring 0, code, readable
    db 11001111b        ; флаги: 4K, 32-бит
    db 0x00             ; база (старш)

gdt_data:               ; селектор 0x10
    dw 0xFFFF
    dw 0x0000
    db 0x00
    db 10010010b        ; доступ: present, ring 0, data, writable
    db 11001111b
    db 0x00

gdt_end:

gdt_descriptor:
    dw gdt_end - gdt_start - 1
    dd gdt_start

; ============================================================
; Обработчики ошибок (только в реальном режиме!)
; ============================================================
disk_error:
    mov si, msg_disk_err
    call print_string
    jmp $

kernel_error:
    mov si, msg_kern_err
    call print_string
    jmp $

; ============================================================
; Заполнение до 510 байт и подпись
; ============================================================
times 510 - ($ - $$) db 0
dw 0xAA55
