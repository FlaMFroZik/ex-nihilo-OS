; ============================================================
; hal/ata.asm — драйвер ATA PIO (LBA28) для primary master
;
; Зачем: файловой системе нужно читать и писать секторы из
; защищённого режима, где BIOS (INT 13h) недоступен.
;
; Почему PIO: команды 0x20 (read) и 0x30 (write) переносят данные
; через порт данных REP INSW/OUTSW. Никакого DMA, PRDT и физических
; адресов под выравнивание — для 16 МиБ образа скорость не важна.
;
; Прерывания не используются: после каждой команды опрашивается
; регистр состояния (BSY/DRQ/ERR). Все ожидания ограничены счётчиком,
; иначе при отсутствии диска (регистр статуса читается как 0xFF)
; цикл не завершится никогда.
;
; Соглашение: cdecl, сохраняются EBX/ESI/EDI/EBP.
; ============================================================
[BITS 32]

IO_BASE   equ 0x1F0
CTRL_BASE equ 0x3F6

REG_DATA      equ 0
REG_SECCOUNT  equ 2
REG_LBA0      equ 3
REG_LBA1      equ 4
REG_LBA2      equ 5
REG_DRIVE     equ 6
REG_STATUS    equ 7
REG_COMMAND   equ 7

ST_BSY  equ 0x80
ST_DRDY equ 0x40
ST_DF   equ 0x20
ST_DRQ  equ 0x08
ST_ERR  equ 0x01

CMD_READ     equ 0x20
CMD_WRITE    equ 0x30
CMD_IDENTIFY equ 0xEC

section .bss
alignb 4
ata_present:      resd 1
ata_sector_count: resd 1

section .text

; ------------------------------------------------------------
; io_wait_400ns — пауза после выбора регистра.
; Четыре чтения альтернативного регистра статуса (0x3F6) дают
; те самые ~400 нс, которых требует спецификация ATA.
; ------------------------------------------------------------
global io_wait_400ns
io_wait_400ns:
    push edx
    push eax
    mov dx, CTRL_BASE
    in al, dx
    in al, dx
    in al, dx
    in al, dx
    pop eax
    pop edx
    ret

; ------------------------------------------------------------
; int ata_wait_ready(void) — дождаться !BSY && DRDY && !ERR.
; 1 — диск готов, 0 — ошибка или таймаут.
; ------------------------------------------------------------
global ata_wait_ready
ata_wait_ready:
    push ebx
    push ecx
    push edx
    mov ecx, 1000000
.poll:
    ; ВАЖНО: читаем в отдельный регистр, а не в AL. Иначе от значения,
    ; которое вызывающий передал в AL, остаётся младший байт (после
    ; IDENTIFY это 0xEC), и проверки битов ниже работают с мусором.
    mov dx, IO_BASE + REG_STATUS
    in al, dx
    mov bl, al
    test bl, ST_BSY
    jz .idle
    loop .poll
    xor eax, eax                ; BSY не снялся — устройства нет
    jmp .done
.idle:
    test bl, ST_ERR
    jnz .bad
    test bl, ST_DRDY
    jz .bad
    mov eax, 1
    jmp .done
.bad:
    xor eax, eax
.done:
    pop edx
    pop ecx
    pop ebx
    ret

; ------------------------------------------------------------
; int ata_wait_drq(void) — дождаться готовности к обмену данными.
; ------------------------------------------------------------
ata_wait_drq:
    push ebx
    push ecx
    push edx
    mov ecx, 1000000
.poll:
    mov dx, IO_BASE + REG_STATUS
    in al, dx
    mov bl, al                  ; AL не портим — см. ata_wait_ready
    test bl, ST_BSY
    jnz .next
    test bl, ST_ERR | ST_DF
    jnz .bad
    test bl, ST_DRQ
    jnz .ok
.next:
    loop .poll
.bad:
    xor eax, eax
    jmp .done
.ok:
    mov eax, 1
.done:
    pop edx
    pop ecx
    pop ebx
    ret

; ------------------------------------------------------------
; int ata_init(void) — 1, если primary master — ATA-диск.
; ------------------------------------------------------------
global ata_init
ata_init:
    pushad

    mov dword [ata_present], 0
    mov dword [ata_sector_count], 0

    ; программный сброс (SRST, бит 2 регистра управления)
    mov dx, CTRL_BASE
    mov al, 0x04
    out dx, al
    call io_wait_400ns
    xor al, al
    out dx, al
    call io_wait_400ns

    ; выбрать master
    mov dx, IO_BASE + REG_DRIVE
    mov al, 0xA0
    out dx, al
    call io_wait_400ns

    ; обнулить счётчик секторов: по нему BIOS помечает наличие диска
    mov dx, IO_BASE + REG_SECCOUNT
    xor al, al
    out dx, al

    ; IDENTIFY DEVICE
    mov dx, IO_BASE + REG_COMMAND
    mov al, CMD_IDENTIFY
    out dx, al

    ; 0x00 в статусе — устройства на канале нет
    mov dx, IO_BASE + REG_STATUS
    in al, dx
    test al, al
    jz .no_disk

    call ata_wait_ready
    test eax, eax
    jz .no_disk

    ; у ATAPI (CD-ROM) LBA1/LBA2 ненулевые — это не ATA-диск
    mov dx, IO_BASE + REG_LBA1
    in al, dx
    test al, al
    jnz .no_disk
    mov dx, IO_BASE + REG_LBA2
    in al, dx
    test al, al
    jnz .no_disk

    ; 256 слов IDENTIFY; слова 60..61 — число секторов LBA28
    mov dx, IO_BASE + REG_DATA
    xor ecx, ecx
.read_words:
    in ax, dx
    cmp ecx, 60
    jne .not_lo
    movzx eax, ax
    mov [ata_sector_count], eax
.not_lo:
    cmp ecx, 61
    jne .not_hi
    movzx eax, ax
    mov [ata_sector_count + 2], eax
.not_hi:
    inc ecx
    cmp ecx, 256
    jb .read_words

    mov dword [ata_present], 1
    mov eax, 1
    popad
    ret

.no_disk:
    xor eax, eax
    popad
    ret

; ------------------------------------------------------------
; uint32_t ata_sectors(void)
; ------------------------------------------------------------
global ata_sectors
ata_sectors:
    mov eax, [ata_sector_count]
    ret

; ------------------------------------------------------------
; ata_set_lba — записать LBA (EAX) в регистры привода.
; Разрушает EAX, EDX. Возвращает EAX = 1 при успехе.
; ------------------------------------------------------------
ata_set_lba:
    push ebx
    push edx

    mov ebx, eax                        ; EBX = LBA (не портим до конца)

    ; Регистр drive: 0xE0 | биты 24..27 LBA
    mov eax, ebx
    shr eax, 24
    and al, 0x0F
    or al, 0xE0
    mov dx, IO_BASE + REG_DRIVE
    out dx, al
    call io_wait_400ns

    ; Один сектор за операцию
    mov dx, IO_BASE + REG_SECCOUNT
    mov al, 1
    out dx, al

    ; LBA0 / LBA1 / LBA2 — из исходного LBA, а не из уже испорченного AL
    mov eax, ebx
    mov dx, IO_BASE + REG_LBA0
    out dx, al

    mov eax, ebx
    shr eax, 8
    mov dx, IO_BASE + REG_LBA1
    out dx, al

    mov eax, ebx
    shr eax, 16
    mov dx, IO_BASE + REG_LBA2
    out dx, al

    call io_wait_400ns

    pop edx
    pop ebx
    mov eax, 1
    ret

; ------------------------------------------------------------
; int ata_read_sector(uint32_t lba, void *buf)   (lba=EAX, buf=EDX)
; ------------------------------------------------------------
global ata_read_sector
ata_read_sector:
    push ebx
    push esi
    push edi
    push ebp

    ; cdecl: [esp+20] = LBA, [esp+24] = буфер
    ; (4 push = 16 байт + 4 байта адреса возврата = 20)
    mov ebp, [esp + 20]                 ; LBA
    mov edi, [esp + 24]                 ; буфер
    mov eax, ebp
    call ata_set_lba
    test eax, eax
    jz .fail

    mov dx, IO_BASE + REG_COMMAND
    mov al, CMD_READ
    out dx, al

    call io_wait_400ns
    call ata_wait_drq
    test eax, eax
    jz .fail

    mov dx, IO_BASE + REG_DATA
    mov ecx, 256
    cld
    rep insw                            ; 256 слов = 512 байт в ES:EDI

    xor eax, eax
    jmp .done
.fail:
    mov eax, -1
.done:
    pop ebp
    pop edi
    pop esi
    pop ebx
    ret

; ------------------------------------------------------------
; int ata_write_sector(uint32_t lba, const void *buf)
; ------------------------------------------------------------
global ata_write_sector
ata_write_sector:
    push ebx
    push esi
    push edi
    push ebp

    ; cdecl: [esp+20] = LBA, [esp+24] = буфер
    mov ebp, [esp + 20]                 ; LBA
    mov esi, [esp + 24]                 ; буфер
    mov eax, ebp
    call ata_set_lba
    test eax, eax
    jz .fail

    mov dx, IO_BASE + REG_COMMAND
    mov al, CMD_WRITE
    out dx, al

    call io_wait_400ns
    call ata_wait_drq
    test eax, eax
    jz .fail

    mov dx, IO_BASE + REG_DATA
    mov ecx, 256
    cld
    rep outsw

    ; запись закончена, когда снялся BSY
    call ata_wait_ready

    xor eax, eax
    jmp .done
.fail:
    mov eax, -1
.done:
    pop ebp
    pop edi
    pop esi
    pop ebx
    ret

; ------------------------------------------------------------
; int ata_is_present(void)
; ------------------------------------------------------------
global ata_is_present
ata_is_present:
    mov eax, [ata_present]
    ret

section .note.GNU-stack noalloc noexec nowrite progbits
