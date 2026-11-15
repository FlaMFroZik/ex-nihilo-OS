; ============================================================
; boot.asm — загрузочный сектор ex-nihilo-OS
;
; Загружается BIOS'ом по адресу 0x7C00, читает ядро с диска
; (начиная с LBA 1) по физическому адресу 0x10000 и передаёт
; управление в 32-битный защищённый режим.
;
; ВАЖНО: ядро грузится по 0x10000, а НЕ по 0x1000. При загрузке
; по 0x1000 64 сектора (32 КиБ) занимают 0x1000..0x8FFF и
; затирают сам загрузчик (0x7C00), его GDT и стек — после
; int 0x13 выполнять было уже нечего.
; ============================================================
[BITS 16]
[ORG 0x7C00]

KERNEL_SEG      equ 0x1000              ; сегмент загрузки ядра → 0x10000
KERNEL_SECTORS  equ 64                  ; 64 сектора = 32 КиБ
DEFAULT_SPT     equ 18                  ; секторов на дорожку (флоппи)
DEFAULT_HEADS   equ 2                   ; головок (флоппи)
MAX_RETRIES     equ 3

start:
    ; Инициализация сегментов и стека
    cli
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov sp, 0x7C00
    cld                                 ; строковые инструкции — вперёд
    sti

    ; Сохраняем номер загрузочного диска
    mov [boot_drive], dl

    ; Вывод сообщения
    mov si, msg_loading
    call print_string

    call enable_a20
    call detect_geometry

    ; ---- Загрузка ядра ----
    mov word [lba], 1                   ; первый сектор после загрузчика
    mov word [dest_off], 0              ; смещение внутри сегмента
    mov ax, KERNEL_SEG
    mov [dest_seg], ax
    mov di, KERNEL_SECTORS              ; счётчик секторов

.read_loop:
    mov byte [retries], MAX_RETRIES

.retry:
    mov ax, [lba]
    call lba_to_chs                     ; → ch, cl, dh
    mov dl, [boot_drive]
    mov es, [dest_seg]
    mov bx, [dest_off]
    mov ax, 0x0201                      ; ah=02 (чтение), al=1 сектор
    int 0x13
    jnc .read_ok

    ; Ошибка чтения: сбросить дисковую подсистему и попробовать снова
    xor ah, ah                          ; ah=00 — сброс
    mov dl, [boot_drive]
    int 0x13
    dec byte [retries]
    jnz .retry
    jmp disk_error

.read_ok:
    add word [dest_off], 512
    jnz .next_sector
    ; Смещение обнулилось — перешли границу 64 КиБ, двигаем сегмент
    mov ax, [dest_seg]
    add ax, 0x1000                      ; 0x1000 параграфов = 64 КиБ
    mov [dest_seg], ax

.next_sector:
    inc word [lba]
    dec di
    jnz .read_loop

    mov si, msg_ok
    call print_string

    ; ---- Переход в защищённый режим ----
    cli
    lgdt [gdt_descriptor]

    mov eax, cr0
    or eax, 1
    mov cr0, eax

    jmp 0x08:protected_mode

; ============================================================
; Включение линии A20 (быстрый шлюз через порт 0x92)
; ============================================================
enable_a20:
    in al, 0x92
    test al, 2
    jnz .done
    or al, 2
    and al, 0xFE                        ; бит 0 = сброс, его трогать нельзя
    out 0x92, al
.done:
    ret

; ============================================================
; Определение геометрии диска (int 0x13, ah=0x08)
; Без этого чтение по CHS с зашитыми 18 секторами на дорожку
; работает только на флоппи, а на жёстком диске — нет.
; ============================================================
detect_geometry:
    push ax
    push cx
    push dx

    mov byte [spt], DEFAULT_SPT
    mov byte [heads], DEFAULT_HEADS

    mov ah, 0x08
    mov dl, [boot_drive]
    int 0x13
    jc .done

    and cx, 0x003F                      ; биты 0..5 cl — число секторов
    jz .done
    mov [spt], cl

    inc dh                              ; dh — максимальный номер головки
    jz .done
    mov [heads], dh
.done:
    pop dx
    pop cx
    pop ax
    ret

; ============================================================
; LBA → CHS
; вход:  ax = LBA
; выход: ch = цилиндр, cl = сектор, dh = головка
; ============================================================
lba_to_chs:
    push ax
    push bx
    push dx

    xor dx, dx
    movzx cx, byte [spt]
    div cx                              ; ax = LBA / spt, dx = LBA % spt
    inc dx
    mov [cur_sector], dl

    xor dx, dx
    movzx cx, byte [heads]
    div cx                              ; ax = цилиндр, dx = головка
    mov [cur_head], dl
    mov [cur_cyl], al

    pop dx
    pop bx
    pop ax

    mov dh, [cur_head]
    mov cl, [cur_sector]
    mov ch, [cur_cyl]
    ret

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
    cld

    ; Переход к ядру (физический адрес 0x10000)
    jmp 0x10000

; ============================================================
; Данные
; ============================================================
[BITS 16]
boot_drive:  db 0
spt:         db DEFAULT_SPT
heads:       db DEFAULT_HEADS
retries:     db 0
lba:         dw 0
dest_off:    dw 0
dest_seg:    dw 0
cur_sector:  db 0
cur_head:    db 0
cur_cyl:     db 0

msg_loading:  db "Loading kernel...", 0x0D, 0x0A, 0
msg_ok:       db "OK! Entering protected mode...", 0x0D, 0x0A, 0
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
%if ($ - $$) > 510
    %error "boot sector exceeds 510 bytes"
%endif
times 510 - ($ - $$) db 0
dw 0xAA55
