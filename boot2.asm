; ============================================================
; boot2.asm — ВТОРОЙ этап загрузки ex-nihilo-OS
;
; Загружается первым этапом (boot.asm) в 0x7E00 и получает
; управление обычным jmp. Здесь помещается всё, что не влезло в
; 512-байтный сектор:
;   1. опрос карты памяти BIOS (INT 15h, AX=E820h);
;   2. загрузка ядра из секторов KERNEL_LBA.. по адресу 0x10000;
;   3. переход в 32-битный защищённый режим.
;
; Чтение диска — CHS (INT 13h/AH=02h). Подробное объяснение, почему
; LBA (AH=42h) для дискеты не работает, — в шапке boot.asm.
;
; Ограничение адресации: ES:BX собирается без переноса в ES, поэтому
; загрузчик корректен для образов не больше 64 КиБ минус 0x10000 —
; это модель PMM (4 МиБ) покрывает с запасом, а ядро ограничено
; 32 КиБ проверкой в Makefile.
; ============================================================
[BITS 16]
[ORG 0x7E00]

; Общие константы с boot.asm (файлы собираются по отдельности,
; поэтому объявлены в обоих).
STAGE2_SECTORS  equ 2
BOOT_DRIVE_ADDR equ 0x0500              ; сюда boot.asm кладёт номер диска

KERNEL_START    equ 0x10000             ; физический адрес загрузки ядра
KERNEL_SECTORS  equ 64                  ; 64 сектора = 32 КиБ (запас;
                                        ; FAT16 начинается с сектора 64)
KERNEL_LBA      equ 1 + STAGE2_SECTORS  ; ядро сразу после второго этапа
MAX_RETRIES     equ 3

; Геометрия диска больше НЕ задаётся константами: у дискеты и у
; жёсткого диска она разная, и угадать её нельзя. Второй этап
; спрашивает её у BIOS через INT 13h/AH=08h и хранит в spt/heads.

E820_COUNT_ADDR equ 0x0600              ; dword: сколько записей собрано
E820_BUF_ADDR   equ 0x0608              ; 20 записей по 24 байта = 480 Б
E820_MAX        equ 20

    xor ax, ax
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov sp, 0x7C00
    cld

    call detect_geometry                ; геометрия диска от BIOS
    call detect_memory_map

    ; Сообщить, сколько регионов нашлось (одна цифра, 10+ -> '9')
    mov si, msg_e820
    call print_string
    mov al, [e820_count]
    cmp al, 9
    jbe .count_ok
    mov al, 9
.count_ok:
    add al, '0'
    call print_char
    mov si, msg_e820_end
    call print_string

    ; ---- Загрузка ядра ----
    mov word [lba], KERNEL_LBA
    mov word [dest_off], 0
    mov word [dest_seg], KERNEL_START >> 4
    mov di, KERNEL_SECTORS
.kernel_loop:
    call read_sector
    add word [dest_off], 512
    jnz .next
    add word [dest_seg], 0x1000         ; 0x1000 параграфов = 64 КиБ
.next:
    inc word [lba]
    dec di
    jnz .kernel_loop

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
; read_sector — прочитать сектор [lba] по ES:[dest_off]
;
; Разрушает AX, BX, CX, DX. Номер диска берётся из ячейки
; boot_drive, которую заполнил первый этап (адрес 0x7C00+).
; ============================================================
read_sector:
    mov byte [retries], MAX_RETRIES
.retry:
    ; --- LBA -> CHS по геометрии, полученной от BIOS (INT 13h/AH=08h) ---
    mov bx, [spt]
    test bx, bx
    jz .io_error                        ; геометрию узнать не удалось

    ; AX = heads * spt (делитель), DX:AX = LBA
    mov ax, [heads]
    mul word [spt]                      ; DX:AX = heads * spt (влезает в AX)
    mov bx, ax                          ; BX = секторов на цилиндр

    mov ax, [lba]
    xor dx, dx
    div bx                              ; AX = цилиндр, DX = LBA % (heads*spt)
    mov [t_cyl], ax

    mov ax, dx
    xor dx, dx
    div word [spt]                      ; AX = голова, DX = сектор-1
    mov dh, al                          ; DH = голова

    mov cl, dl
    inc cl                              ; CL = сектор (нумерация с 1)
    mov al, [t_cyl + 1]                 ; старшие 2 бита цилиндра
    and al, 3
    shl al, 6
    or cl, al                           ; ...в биты 6-7 регистра CL
    mov ch, [t_cyl]                     ; CH = младшие 8 бит цилиндра

    mov dl, [BOOT_DRIVE_ADDR]
    mov ax, [dest_seg]
    mov es, ax                          ; ES:BX = куда читать
    mov bx, [dest_off]
    mov ax, 0x0201                      ; AH=02h чтение, AL=1 сектор
    int 0x13
    jnc .ok

    xor ah, ah                          ; сброс дисковой подсистемы
    mov dl, [BOOT_DRIVE_ADDR]
    int 0x13
    dec byte [retries]
    jnz .retry

.io_error:
    mov si, msg_disk_err
    call print_string
    jmp $
.ok:
    ret

; ============================================================
; detect_geometry — узнать у BIOS число секторов на дорожку и
; головок (INT 13h/AH=08h).
;
; Без этого LBA→CHS считать нельзя: у дискеты 18/2, у маленького
; IDE-диска в QEMU — 63/16, и ошибка в геометрии означает чтение
; совсем других секторов (в лучшем случае — "Disk read error!").
;
; Результат: spt и heads в памяти. При ошибке BIOS оставляем
; значения по умолчанию (1 сектор, 1 головка) — тогда read_sector
; честно упадёт в сообщение об ошибке, а не прочитает мусор.
; ============================================================
detect_geometry:
    pusha

    mov word [spt], 0
    mov word [heads], 0

    mov ah, 0x08
    mov dl, [BOOT_DRIVE_ADDR]
    xor di, di                          ; старые BIOS: DL = число дисков
    int 0x13
    jc .done

    ; CL[5:0] — максимум секторов, DH — максимум головок
    mov al, cl
    and ax, 0x3F
    mov [spt], ax
    movzx ax, dh
    inc ax                              ; DH — номер последней головки
    mov [heads], ax

.done:
    popa
    ret

; ============================================================
; Карта памяти BIOS: INT 15h, AX = E820h
;
; Это прерывание существует ТОЛЬКО в реальном режиме, поэтому
; опрос делается здесь, в загрузчике, а не в hal/pmm.asm (код
; ядра уже выполняется в защищённом режиме, где int 0x15 привёл
; бы к исключению из-за пустой IDT).
;
; Результат: массив записей по 24 байта в E820_BUF, счётчик
; записей — по адресу E820_COUNT_ADDR. Формат записи (ACPI 3.0):
;   +0  qword base        — начало области
;   +8  qword length      — длина (0 = запись недействительна)
;   +16 dword type        — 1 = доступна для ОС, 2 = занята
;   +20 dword acpi_ext    — расширенные атрибуты (игнорируем)
;
; DX:EAX возвращает сигнатуру 'SMAP', EBX — продолжение нумерации
; (0 — записей больше нет). Записи с базой выше 4 ГиБ не нужны:
; ядро работает без страничной адресации.
; ============================================================
detect_memory_map:
    pusha

    ; ВАЖНО: int 0x15 в реальном режиме НЕ сохраняет BP — в нём BIOS
    ; возвращает флаг конфигурации, а EBX трогает как «продолжение
    ; нумерации». Поэтому счётчик и continuation живут в памяти.
    mov word [e820_count], 0
    mov dword [e820_next], 0
    mov di, E820_BUF_ADDR
.next_entry:
    mov eax, 0xE820
    mov edx, 0x534D4150                 ; 'SMAP'
    mov ecx, 24                         ; размер записи
    mov ebx, [e820_next]
    int 0x15
    jc .done
    cmp eax, 0x534D4150
    jne .done
    jcxz .done

    mov eax, [di + 8]                   ; length == 0 → запись пустая
    or eax, [di + 12]
    jz .skip

    cmp dword [di + 4], 0               ; base >= 4 ГиБ — дальше не нужно
    jne .done

    inc word [e820_count]
    add di, 24
    cmp di, E820_BUF_ADDR + E820_MAX * 24
    jae .done
.skip:
    mov [e820_next], ebx
    test ebx, ebx
    jnz .next_entry
.done:
    movzx eax, word [e820_count]
    mov [E820_COUNT_ADDR], eax          ; dword: старшее слово = 0
    popa
    ret

; ============================================================
; Вывод в текстовом режиме через BIOS
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

    ; Стек, выровненный по 16: 0x90000 кратен 16, and — страховка
    mov esp, 0x90000
    and esp, 0xFFFFFFF0
    cld

    jmp KERNEL_START                    ; вход в ядро (kernel.asm)

; ============================================================
; Данные
; ============================================================
[BITS 16]
dest_off:    dw 0
dest_seg:    dw 0
retries:     db 0
lba:         dw 0
t_cyl:       dw 0
spt:         dw 0
heads:       dw 0

e820_count:  dw 0
e820_next:   dd 0

msg_e820:     db "E820: ", 0
msg_e820_end: db " regions (max 20)", 0x0D, 0x0A, 0
msg_ok:       db "OK, booting", 0x0D, 0x0A, 0
msg_disk_err: db "Disk read error!", 0x0D, 0x0A, 0

; ============================================================
; GDT
; ============================================================
gdt_start:
    dq 0                                ; нулевой дескриптор (обязателен)
gdt_code:
    dq 0x00CF9A000000FFFF               ; code: база 0, лимит 4 ГиБ, r-x
gdt_data:
    dq 0x00CF92000000FFFF               ; data: база 0, лимит 4 ГиБ, rw-
gdt_end:

gdt_descriptor:
    dw gdt_end - gdt_start - 1
    dw gdt_start                        ; ORG 0x7E00 → адрес уже абсолютный

%if ($ - $$) > 1024
    %error "boot2.asm: second stage exceeds 1024 bytes"
%endif
times 1024 - ($ - $$) db 0
