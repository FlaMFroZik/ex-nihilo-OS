[BITS 16]
[ORG 0x7C00]

start:
    ; Инициализация сегментов
    cli
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov sp, 0x7C00
    sti

    ; Сохраняем номер загрузочного диска
    mov [boot_drive], dl

    ; Вывод сообщения
    mov si, msg_loading
    call print_string

    ; Включить A20
    in al, 0x92
    or al, 2
    out 0x92, al

    ; Загрузка ядра: 64 сектора (32 КБ)
    mov ah, 0x02
    mov al, 64              ; 64 сектора
    mov ch, 0
    mov cl, 2
    mov dh, 0
    mov dl, [boot_drive]
    mov bx, 0x1000
    int 0x13
    jc disk_error

    mov si, msg_ok
    call print_string

    ; Переход в защищённый режим
    cli
    lgdt [gdt_descriptor]

    mov eax, cr0
    or eax, 1
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
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax

    ; Стек, выровненный по 16
    mov esp, 0x90000
    and esp, 0xFFFFFFF0

    ; Переход к ядру
    jmp 0x1000

; ============================================================
; Данные
; ============================================================
[BITS 16]
boot_drive:  db 0

msg_loading: db "Loading kernel...", 0x0D, 0x0A, 0
msg_ok:      db "OK! Entering protected mode...", 0x0D, 0x0A, 0
msg_disk_err: db "Disk read error!", 0x0D, 0x0A, 0

; ============================================================
; GDT
; ============================================================
align 8
gdt_start:
    dq 0

gdt_code:
    dw 0xFFFF
    dw 0x0000
    db 0x00
    db 10011010b
    db 11001111b
    db 0x00

gdt_data:
    dw 0xFFFF
    dw 0x0000
    db 0x00
    db 10010010b
    db 11001111b
    db 0x00

gdt_end:

gdt_descriptor:
    dw gdt_end - gdt_start - 1
    dd gdt_start

; ============================================================
; Ошибки
; ============================================================
disk_error:
    mov si, msg_disk_err
    call print_string
    jmp $

; ============================================================
; Подпись
; ============================================================
times 510 - ($ - $$) db 0
dw 0xAA55
