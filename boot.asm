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

KERNEL_START    equ 0x10000             ; физический адрес загрузки ядра
KERNEL_SECTORS  equ 64                  ; 64 сектора = 32 КиБ
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

    ; Вывод сообщения: "K" — начинаем загрузку ядра
    mov si, msg_loading
    call print_string

    call enable_a20
    call detect_memory_map              ; E820: карта памяти от BIOS

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
    ; INT 13h/AH=42h (LBA extended read) через Disk Address Packet:
    ; не требует знания геометрии CHS и работает и на HDD, и на CD/DVD.
    mov word [lba], 1                   ; первый сектор после загрузчика
    mov di, KERNEL_SECTORS              ; счётчик секторов

.read_loop:
    mov byte [retries], MAX_RETRIES

.retry:
    ; Собрать DAP: смещение и сегмент из физического адреса dest.
    ; dest = KERNEL_START + 512*i, сегмент = dest>>4, смещение = dest&0xF.
    mov eax, [dest]
    mov edx, eax
    and ax, 0x000F                      ; смещение внутри параграфа
    mov [dap + 4], ax
    shr edx, 4                          ; сегмент
    mov [dap + 6], dx
    mov ax, [lba]
    mov [dap + 8], ax                   ; LBA, младшие 16 бит
    mov word [dap + 10], 0              ; старшие биты LBA = 0

    mov si, dap                         ; DS:SI -> Disk Address Packet
    mov ah, 0x42
    mov dl, [boot_drive]
    int 0x13
    jnc .read_ok

    ; Ошибка чтения: сбросить дисковую подсистему и повторить
    xor ah, ah                          ; AH=00 — сброс
    mov dl, [boot_drive]
    int 0x13
    dec byte [retries]
    jnz .retry
    jmp disk_error

.read_ok:
    add dword [dest], 512               ; следующая цель

.next_sector:
    inc word [lba]
    dec di
    jnz .read_loop

    mov si, msg_ok                      ; ядро на месте — в защищённый режим
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
; Карта памяти BIOS: INT 15h, AX = E820h
;
; Это прерывание существует ТОЛЬКО в реальном режиме, поэтому
; опрос делается здесь, в загрузчике, а не в hal/pmm.asm (код
; ядра уже выполняется в защищённом режиме, где int 0x15 привёл
; бы к исключению из-за пустой IDT).
;
; Результат: массив записей по 24 байта в E820_BUF (адрес E820_BUF_ADDR),
; счётчик записей — в e820_count. Формат записи (ACPI 3.0):
;   +0  qword base        — начало области
;   +8  qword length      — длина (0 = запись недействительна)
;   +16 dword type        — 1 = доступна для ОС, 2 = занята/зарезервирована
;   +20 dword acpi_ext    — расширенные атрибуты (игнорируем)
;
; DX:EAX возвращает "сигнатуру" 'SMAP', EBX — продолжение нумерации
; (0 — записей больше нет). Записи обрезаются по концу первого
; мегабайта: всё, что выше, ядро всё равно не использует — оно
; работает без страничной адресации, а модель PMM = 4 МиБ.
; ============================================================
E820_COUNT_ADDR equ 0x0600              ; dword: сколько записей собрано
E820_BUF_ADDR   equ 0x0608              ; 20 записей по 24 байта = 480 Б
E820_MAX        equ 20

detect_memory_map:
    pusha

    ; ВАЖНО: int 0x15 в реальном режиме НЕ сохраняет BP — в нём BIOS
    ; возвращает флаг конфигурации, а EBX трогает как «продолжение
    ; нумерации». Поэтому счётчик записей и continuation живут в
    ; памяти, а не в регистрах: иначе часть карты терялась.
    mov word [e820_count], 0
    mov dword [e820_next], 0
    mov di, E820_BUF_ADDR
.next_entry:
    mov eax, 0xE820
    mov edx, 0x534D4150                 ; 'SMAP'
    mov ecx, 24                         ; размер записи
    mov ebx, [e820_next]                ; продолжить с того же места
    int 0x15
    jc .done                            ; ошибка/конец — прекращаем
    cmp eax, 0x534D4150                 ; BIOS должен вернуть 'SMAP'
    jne .done
    jcxz .done                          ; пустая запись — прекращаем

    ; длина 0 — запись недействительна, пропускаем
    mov eax, [di + 8]
    or eax, [di + 12]
    jz .skip

    ; базовый адрес выше 4 ГиБ — область ядру не нужна, но это
    ; не конец списка: записи идут по возрастанию адреса,
    ; поэтому просто останавливаемся.
    cmp dword [di + 4], 0               ; старшие 32 бита base
    jne .done

    inc word [e820_count]
    add di, 24
    cmp di, E820_BUF_ADDR + E820_MAX * 24
    jae .done
.skip:
    mov [e820_next], ebx                ; BIOS вернул новое продолжение
    test ebx, ebx
    jnz .next_entry                     ; ebx = 0 — записи кончились
.done:
    movzx eax, word [e820_count]
    mov [E820_COUNT_ADDR], eax          ; dword: старшее слово = 0
    popa
    ret

; ============================================================
; 16-битный вывод строки
; ============================================================
print_char:
    pusha
    mov ah, 0x0E
    int 0x10
    popa
    ret

print_string:
    pusha
.next_char:
    lodsb
    test al, al
    jz .end
    call print_char
    jmp .next_char
.end:
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
retries:     db 0
lba:         dw 0
dest:        dd KERNEL_START            ; куда читать (0x10000)

; Disk Address Packet для INT 13h/AH=42h (16 байт)
align 4
dap:
    db 16                                ; размер пакета
    db 0                                 ; зарезервировано
    dw 1                                 ; сколько секторов читать
    dw 0                                 ; смещение в сегменте (заполняется)
    dw 0                                 ; сегмент (заполняется)
    dq 1                                 ; LBA (заполняется)
e820_count:  dw 0                       ; счётчик записей (BP не сохраняется!)
e820_next:   dd 0                       ; continuation для следующего вызова

; Сообщения. Их длина ограничена свободным местом загрузочного
; сектора (512 Б), поэтому текст компактный, но осмысленный.
msg_loading:  db "Loading kernel...", 0x0D, 0x0A, 0
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
    dw gdt_start - 1

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
