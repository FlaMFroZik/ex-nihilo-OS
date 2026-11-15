; hal/pmm.asm — физический менеджер памяти (PMM) со спинлоком
; NASM, 32-bit
;
; Модель: 1024 страницы по 4 КиБ = 4 МиБ адресного пространства.
; 1 бит в bitmap = 1 страница: 1 — занята, 0 — свободна.

%define PAGE_COUNT      1024
%define BITMAP_BYTES    (PAGE_COUNT / 8)
; Всё, что ниже 1 МиБ и не занято ядром, трогать нельзя:
;   0x80000..0x8FFFF — стек ядра (растёт вниз от 0x90000),
;   0x9FC00..0x9FFFF — EBDA,
;   0xA0000..0xBFFFF — видеопамять VGA,
;   0xF0000..0xFFFFF — BIOS ROM.
; Проще зарезервировать один непрерывный кусок 0x80000..0xFFFFF.
%define LOW_RESERVED_FIRST  (0x80000 / 4096)    ; 128
%define LOW_RESERVED_LAST   (0xFFFFF / 4096)    ; 255

section .bss
    alignb 4
    bitmap      resb BITMAP_BYTES   ; 1024 страницы = 4 МБ
    pmm_lock    resd 1              ; спинлок

section .text
extern spin_lock
extern spin_unlock
extern kernel_end               ; символ из linker.ld: конец образа ядра

; ---------------------------------------------------------
; pmm_mark_used_range
;   вход: eax = первая страница, ebx = последняя (включительно)
;   Вызывать только без захваченного лока (из pmm_init).
; ---------------------------------------------------------
pmm_mark_used_range:
    push eax
    push ebx

    cmp ebx, PAGE_COUNT - 1
    jbe .clamped
    mov ebx, PAGE_COUNT - 1         ; не выходим за модель памяти
.clamped:
    cmp eax, ebx
    ja .done

.mark_loop:
    ; bts с регистровым операндом трактует eax как НОМЕР бита
    ; во всей области памяти, то есть bitmap[eax / 8] |= 1 << (eax % 8)
    bts [bitmap], eax

    inc eax
    cmp eax, ebx
    jbe .mark_loop
.done:
    pop ebx
    pop eax
    ret

; ---------------------------------------------------------
; void pmm_init(void)
;
; Освобождает весь bitmap и резервирует:
;   * страницы 0..kernel_end — загрузчик, код и данные ядра,
;     таблицы BIOS, EBDA и прочая низкая память;
;   * 0x80000..0xFFFFF — стек ядра, EBDA, видеопамять и BIOS ROM.
; ---------------------------------------------------------
global pmm_init
pmm_init:
    push eax
    push ebx
    push ecx
    push edi

    mov dword [pmm_lock], 0         ; лок свободен

    ; очистить bitmap (DF должен быть 0 — его выставляет kernel.asm)
    mov edi, bitmap
    mov ecx, BITMAP_BYTES
    xor eax, eax
    rep stosb

    ; --- зарезервировать всё до конца образа ядра включительно ---
    mov ebx, kernel_end
    add ebx, 4095
    shr ebx, 12                     ; первая страница за ядром
    dec ebx                         ; последняя занятая страница
    xor eax, eax
    call pmm_mark_used_range

    ; --- зарезервировать стек, EBDA, видеопамять и BIOS ROM ---
    mov eax, LOW_RESERVED_FIRST
    mov ebx, LOW_RESERVED_LAST
    call pmm_mark_used_range

    pop edi
    pop ecx
    pop ebx
    pop eax
    ret

; ---------------------------------------------------------
; void *pmm_alloc(void)
; Возвращает физический адрес свободной страницы или NULL.
; ---------------------------------------------------------
global pmm_alloc
pmm_alloc:
    push ebx
    push ecx
    push edx

    push pmm_lock
    call spin_lock
    add esp, 4

    xor ecx, ecx                    ; ecx = индекс страницы

.scan:
    cmp ecx, PAGE_COUNT
    jae .nomem

    mov ebx, ecx
    mov edx, ecx
    shr ebx, 3                      ; номер байта bitmap
    and edx, 7                      ; номер бита в байте
    movzx eax, byte [bitmap + ebx]
    bt eax, edx
    jc .next                        ; занята — искать дальше

    ; свободна — занять (bit test and set по индексу страницы в ecx)
    bts [bitmap], ecx

    ; адрес = индекс * 4096
    mov eax, ecx
    shl eax, 12

    push eax
    push pmm_lock
    call spin_unlock
    add esp, 4
    pop eax

    pop edx
    pop ecx
    pop ebx
    ret

.next:
    inc ecx
    jmp .scan

.nomem:
    push pmm_lock
    call spin_unlock
    add esp, 4

    xor eax, eax
    pop edx
    pop ecx
    pop ebx
    ret

; ---------------------------------------------------------
; void pmm_free(void *addr)
;
; Освобождает страницу. Игнорирует NULL, невыровненные адреса,
; адреса вне модели памяти, страницы, которые не были выделены,
; а также зарезервированные области (ядро, VGA, BIOS ROM) —
; иначе можно «освободить» код ядра или видеопамять.
; ---------------------------------------------------------
global pmm_free
pmm_free:
    push ebx

    mov ebx, [esp + 8]              ; addr
    test ebx, ebx
    jz .done

    test ebx, 4095                  ; должен быть выровнен по странице
    jnz .done

    shr ebx, 12                     ; индекс страницы
    cmp ebx, PAGE_COUNT
    jae .done

    push edx

    push pmm_lock
    call spin_lock
    add esp, 4

    ; страница не была выделена — ничего не делаем
    bt [bitmap], ebx
    jnc .unlock

    ; не освобождаем низкую память (загрузчик, ядро, BIOS, EBDA)
    mov eax, kernel_end
    add eax, 4095
    shr eax, 12                     ; первая свободная страница
    cmp ebx, eax
    jb .unlock

    ; не освобождаем стек, EBDA, видеопамять и BIOS ROM
    cmp ebx, LOW_RESERVED_FIRST
    jb .really_free
    cmp ebx, LOW_RESERVED_LAST
    jbe .unlock

.really_free:
    btr [bitmap], ebx               ; сбросить бит (bit test and reset)

.unlock:
    push pmm_lock
    call spin_unlock
    add esp, 4

    pop edx
.done:
    pop ebx
    ret

; ---------------------------------------------------------
; uint32_t pmm_free_pages(void) — сколько страниц свободно
; ---------------------------------------------------------
global pmm_free_pages
pmm_free_pages:
    push ebx
    push ecx
    push edx

    push pmm_lock
    call spin_lock
    add esp, 4

    xor ecx, ecx                    ; номер байта bitmap
    xor edx, edx                    ; счётчик свободных страниц

.count_byte:
    cmp ecx, BITMAP_BYTES
    jae .count_done

    movzx eax, byte [bitmap + ecx]
    xor eax, 0xFF                   ; свободные биты становятся единицами

.count_bits:
    test eax, eax
    jz .next_byte
    mov ebx, eax
    dec ebx
    and ebx, eax                    ; сбросить младший установленный бит
    inc edx
    mov eax, ebx
    jmp .count_bits

.next_byte:
    inc ecx
    jmp .count_byte

.count_done:
    push edx
    push pmm_lock
    call spin_unlock
    add esp, 4
    pop eax                         ; вернуть счётчик

    pop edx
    pop ecx
    pop ebx
    ret

; помечаем стек неисполняемым (убирает warning линкера)
section .note.GNU-stack noalloc noexec nowrite progbits
