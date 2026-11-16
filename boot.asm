; ============================================================
; boot.asm — ПЕРВЫЙ этап загрузки ex-nihilo-OS (один сектор, 512 Б)
;
; BIOS загружает этот сектор по адресу 0x7C00. Задача этапа —
; минимальная и потому максимально надёжная:
;   1. настроить сегменты и стек;
;   2. включить линию A20;
;   3. прочитать второй этап (секторы 1..STAGE2_SECTORS) в 0x7E00;
;   4. передать ему управление.
;
; Почему этапов два: в 512 байт не помещаются одновременно опрос
; E820, длинные сообщения и загрузка ядра. Второй этап живёт в
; собственных секторах диска и имеет 1024 байта — там места хватает.
;
; ПОЧЕМУ CHS (INT 13h/AH=02h), А НЕ INT 13h/AH=42h (LBA):
;   образ грузится как ДИСКЕТА (if=floppy / -fda). В SeaBIOS
;   (src/disk.c, функция floppy_13) для дискет разрешены только
;   AH=00h,01h,02h,03h,04h,05h,08h,15h,16h — всё остальное уходит
;   в disk_13XX() = «не реализовано» с CF=1. Поэтому LBA-чтение
;   дискеты всегда давало "Disk read error!".
;   ВАЖНО про геометрию: CHS зависит от того, какую геометрию
;   использует BIOS, а она у дискеты и у жёсткого диска разная
;   (QEMU для маленького IDE-диска отдаёт 16 головок / 63 сектора).
;   Поэтому первый этап читает только секторы 1 и 2 и вообще не
;   делит: для любой геометрии это C=0, H=0, S=LBA+1. Геометрию
;   второй этап спрашивает у BIOS (INT 13h/AH=08h) — см. boot2.asm.
; ============================================================
[BITS 16]
[ORG 0x7C00]

STAGE2_START    equ 0x7E00              ; сразу за загрузочным сектором
STAGE2_SECTORS  equ 4                   ; второй этап занимает 4 сектора
MAX_RETRIES     equ 3
BOOT_DRIVE_ADDR equ 0x0500              ; номер диска для второго этапа

; Геометрия дискеты 1.44 МБ
SPT             equ 18                  ; секторов на дорожку
HEADS           equ 2                   ; головок
TRACK_SECTORS   equ SPT * HEADS         ; 36 секторов на цилиндр

start:
    cli
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov sp, 0x7C00
    cld                                 ; lodsb/stosb — только вперёд
    sti

    mov [BOOT_DRIVE_ADDR], dl           ; BIOS передал номер диска в DL

    mov si, msg_loading
    call print_string

    call enable_a20

    ; ---- Чтение второго этапа: секторы 1..STAGE2_SECTORS → 0x7E00 ----
    mov word [lba], 1
    mov word [dest_off], STAGE2_START
    mov di, STAGE2_SECTORS
.stage2_loop:
    call read_sector
    add word [dest_off], 512
    inc word [lba]
    dec di
    jnz .stage2_loop

    jmp STAGE2_START                    ; дальше — boot2.asm

; ============================================================
; read_sector — прочитать сектор [lba] по адресу 0000:[dest_off]
;
; Разрушает AX, BX, CX, DX. При исчерпании попыток печатает
; сообщение и останавливается — возвращаться некуда.
; ============================================================
read_sector:
    mov byte [retries], MAX_RETRIES
.retry:
    ; --- LBA → CHS ---
    mov ax, [lba]
    xor dx, dx
    mov bx, TRACK_SECTORS
    div bx                              ; AX = цилиндр, DX = LBA % 36
    mov [t_cyl], ax
    mov ax, dx
    xor dx, dx
    mov bx, SPT
    div bx                              ; AX = голова, DX = сектор-1
    mov dh, al                          ; DH = голова
    mov cl, dl
    inc cl                              ; CL = сектор (с 1)
    mov al, [t_cyl + 1]                 ; старшие 2 бита цилиндра
    and al, 3
    shl al, 6
    or cl, al                           ; ...в биты 6-7 регистра CL
    mov ch, [t_cyl]                     ; CH = младшие 8 бит цилиндра

    mov dl, [BOOT_DRIVE_ADDR]
    mov bx, [dest_off]
    mov ax, 0x0201                      ; AH=02h чтение, AL=1 сектор
    int 0x13
    jnc .ok

    ; Сброс дисковой подсистемы и повтор
    xor ah, ah
    mov dl, [BOOT_DRIVE_ADDR]
    int 0x13
    dec byte [retries]
    jnz .retry

    mov si, msg_disk_err
    call print_string
    jmp $                               ; грузиться нечем — стоп
.ok:
    ret

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
; Вывод в текстстовом режиме через BIOS
; ============================================================
print_char:
    pusha
    mov ah, 0x0E
    int 0x10
    popa
    ret

print_string:
    pusha
.next:
    lodsb
    test al, al
    jz .done
    call print_char
    jmp .next
.done:
    popa
    ret

; ============================================================
; Данные
; ============================================================
retries:     db 0
lba:         dw 0
dest_off:    dw 0
t_cyl:       dw 0

msg_loading:  db "Loading...", 0x0D, 0x0A, 0
msg_disk_err: db "Disk read error!", 0x0D, 0x0A, 0

%if ($ - $$) > 510
    %error "boot.asm: first stage exceeds 510 bytes"
%endif
times 510 - ($ - $$) db 0
dw 0xAA55
