; ============================================================
; hal/pmm.asm — физический менеджер памяти (PMM)
;
; NASM, 32-bit, защищённый режим.
;
; Источник данных о памяти — карта E820, которую BIOS отдаёт по
; INT 15h/AX=E820h. Это прерывание существует ТОЛЬКО в реальном
; режиме, поэтому опрос делает загрузчик (boot.asm), а здесь мы
; карту разбираем. Здесь int 0x15 выполнить нельзя: в защищённом
; режиме он уйдёт в IDT, которой у ядра ещё нет.
;
; Раскладка карты (см. boot.asm):
;   0x0600 — dword: количество записей
;   0x0608 — массив записей по 24 байта (ACPI 3.0):
;            +0 qword base, +8 qword length, +16 dword type
;
; Принцип работы:
;   1. Из карты E820 берём только области type == 1 (доступна ОС),
;      не выходя за предел 4 ГиБ (у нас 32-битные адреса).
;   2. Всю остальную память считаем занятой (fail closed).
;   3. Размер bitmap — на всю доступную память, поэтому «вся ОЗУ»
;      ограничена лишь объёмом установленной памяти.
;
; Сам bitmap живёт в .bss ядра (kernel_end уже зарезервирован),
; то есть занимать под него страницы у самого себя не нужно.
; ============================================================

%define PAGE_SHIFT      12
%define PAGE_SIZE       4096
%define BITS_PER_ENTRY  32                          ; размер слова bitmap

; Адреса карты и ячейки BIOS можно переопределить при сборке
; (-DE820_COUNT_ADDR=...): это нужно host-тестам, где 0x600 не отображён.
; В этом случае адреса — символы .bss теста, их надо объявить внешними.
%ifdef PMM_HOST_TEST
extern test_e820_count
extern test_e820_buf
extern test_bios_mem_kb
%endif
%ifndef E820_COUNT_ADDR
%define E820_COUNT_ADDR 0x0600
%endif
%ifndef E820_BUF_ADDR
%define E820_BUF_ADDR   0x0608
%endif
%ifndef BIOS_MEM_KB_ADDR
%define BIOS_MEM_KB_ADDR 0x0413
%endif
%define E820_MAX        20
%define E820_ENTRY_SIZE 24
%define E820_TYPE_FREE  1

%define BITMAP_MAX_BYTES (1024 * 1024 / 8)          ; 1 МиБ => 4 ГиБ памяти

%define KERNEL_STACK_TOP 0x90000                    ; см. kernel.asm

section .bss
    alignb 4
    bitmap          resb BITMAP_MAX_BYTES   ; 1 бит = 1 страница
    pmm_lock        resd 1
    page_limit      resd 1                  ; сколько страниц покрывает bitmap
    free_pages      resd 1                  ; свободных страниц (для pmm_free_pages)
    e820_highest    resd 1                  ; максимальный адрес доступной памяти
    e820_entries    resd 1                  ; сколько записей реально прочитано
    e820_usable     resd 1                  ; сколько из них type == 1

section .text
extern spin_lock
extern spin_unlock
extern kernel_end

; ---------------------------------------------------------
; pmm_set_bit / pmm_clear_bit / pmm_test_bit
;   eax = номер страницы
;   Меняют только память bitmap, границы уже проверены вызывающим.
; ---------------------------------------------------------
pmm_set_bit:
    bts [bitmap], eax
    ret

pmm_clear_bit:
    btr [bitmap], eax
    ret

pmm_test_bit:
    bt [bitmap], eax
    ret

; ---------------------------------------------------------
; pmm_mark_range
;   вход: eax = первая страница, ebx = последняя (включительно)
;   Без лока: вызывается только из pmm_init.
; ---------------------------------------------------------
pmm_mark_range:
    push eax
    push ebx

    ; диапазон целиком за пределом модели памяти — нечего делать
    ; (важно: page_limit может быть 0, тогда cmp ниже дал бы underflow)
    cmp eax, ebx
    ja .done
    cmp eax, [page_limit]
    jae .done

    cmp ebx, [page_limit]
    jb .clamped
    mov ebx, [page_limit]
    dec ebx                                     ; не выходим за bitmap
.clamped:
    cmp eax, ebx
    ja .done
.loop:
    bts [bitmap], eax
    inc eax
    cmp eax, ebx
    jbe .loop
.done:
    pop ebx
    pop eax
    ret

; ---------------------------------------------------------
; pmm_clear_range
;   вход: eax = первая страница, ebx = последняя (включительно)
; ---------------------------------------------------------
pmm_clear_range:
    push eax
    push ebx

    cmp eax, ebx
    ja .done
    cmp eax, [page_limit]
    jae .done

    cmp ebx, [page_limit]
    jb .clamped
    mov ebx, [page_limit]
    dec ebx
.clamped:
    cmp eax, ebx
    ja .done
.loop:
    btr [bitmap], eax
    inc eax
    cmp eax, ebx
    jbe .loop
.done:
    pop ebx
    pop eax
    ret

; ---------------------------------------------------------
; pmm_count_free — пересчитать free_pages по bitmap (без лока)
;
; Идём страница за страницей: медленнее, чем считать биты в слове,
; зато граница page_limit обрабатывается без масок и сдвигов.
; ---------------------------------------------------------
pmm_count_free:
    push eax
    push ecx

    xor ecx, ecx                    ; номер страницы
    xor eax, eax                    ; счётчик свободных
    mov edx, [page_limit]

.loop:
    cmp ecx, edx
    jae .done

    push eax
    push ecx
    mov eax, ecx
    call pmm_test_bit
    pop ecx
    pop eax

    jc .next                        ; занята
    inc eax
.next:
    inc ecx
    jmp .loop

.done:
    mov [free_pages], eax
    pop ecx
    pop eax
    ret

; ---------------------------------------------------------
; void pmm_init(void)
;
; Строит bitmap по карте E820 из загрузчика.
; ---------------------------------------------------------
global pmm_init
pmm_init:
    push eax
    push ebx
    push ecx
    push edx
    push esi
    push edi

    mov dword [pmm_lock], 0

    ; ---- 1. Всё занято: пока карта не разобрана, память не выдаём ----
    mov edi, bitmap
    mov ecx, BITMAP_MAX_BYTES / 4
    mov eax, 0xFFFFFFFF
    rep stosd

    ; ---- 2. Разобрать карту E820 ----
    mov esi, [E820_COUNT_ADDR]
    mov [e820_entries], esi
    mov dword [e820_usable], 0
    mov dword [e820_highest], 0
    mov dword [page_limit], 0

    test esi, esi
    jz .no_e820                     ; загрузчик не смог опросить BIOS
    cmp esi, E820_MAX
    jbe .count_ok
    mov esi, E820_MAX
    mov [e820_entries], esi
.count_ok:
    mov edi, E820_BUF_ADDR

.entry_loop:
    test esi, esi
    jz .entries_done

    ; длина области: старшее слово должно быть нулевым (иначе > 4 ГиБ)
    cmp dword [edi + 12], 0
    jne .next_entry
    mov ebx, [edi + 8]              ; младшее слово длины
    test ebx, ebx
    jz .next_entry                  ; пустая область

    ; база: старшее слово должно быть нулевым
    mov eax, [edi + 4]
    test eax, eax
    jnz .next_entry

    ; область должна быть доступна ОС
    cmp dword [edi + 16], E820_TYPE_FREE
    jne .next_entry

    ; считать адрес конца области (с защитой от переполнения)
    mov eax, [edi]                  ; base (младшее слово)
    mov edx, eax
    add edx, ebx                    ; end = base + length
    jc .clamp_4g
    jmp .end_ready
.clamp_4g:
    mov edx, 0xFFFFF000             ; 4 ГиБ минус страница
.end_ready:

    ; запомнить самый высокий доступный адрес
    cmp edx, [e820_highest]
    jbe .no_highest
    mov [e820_highest], edx
.no_highest:

    cmp edx, eax
    jbe .next_entry                 ; пустой диапазон — запись ничего не даёт

    ; запись реально принесла память — учитываем её в статистике
    inc dword [e820_usable]

    ; ---- освободить страницы [base/4096, (end-1)/4096] ----
    ; Здесь нужен непрерывный диапазон до конца области, поэтому
    ; освобождаем через btr напрямую. Границу модели (page_limit)
    ; поднимаем сразу: биты «занято» за старым пределом уже стоят,
    ; в новой области их не было, так что состояние корректно.
    push esi
    push edi

    mov ebx, edx
    dec ebx
    shr ebx, PAGE_SHIFT             ; последняя страница области
    inc ebx                         ; сколько страниц до конца области
    cmp ebx, [page_limit]
    jbe .limit_ok
    mov [page_limit], ebx           ; страница №ebx ещё не входит в модель
.limit_ok:
    shr eax, PAGE_SHIFT             ; первая страница области

.free_loop:
    btr [bitmap], eax
    inc eax
    cmp eax, ebx
    jb .free_loop

    pop edi
    pop esi

.next_entry:
    add edi, E820_ENTRY_SIZE
    dec esi
    jmp .entry_loop
.entries_done:
    jmp .after_map

; ---------------------------------------------------------
.no_e820:
; BIOS не дал карту (или прерывание запрещено). Опираемся на
; нижнюю границу памяти от BIOS (int 12h кладёт её в 0x413, в КиБ)
; и считаем доступным весь диапазон до неё.
    movzx eax, word [BIOS_MEM_KB_ADDR]
    shl eax, 10                     ; КиБ → байты
    test eax, eax
    jnz .have_low
    mov eax, 0x100000               ; совсем ничего не известно — 1 МиБ
.have_low:
    mov [e820_highest], eax
    mov ebx, eax
    dec ebx
    shr ebx, PAGE_SHIFT
    inc ebx
    mov [page_limit], ebx
    xor eax, eax
    call pmm_clear_range

.after_map:

    ; ---- 3. Резервируем то, что отдавать нельзя ----
    ; a) всё до конца образа ядра (загрузчик, код, данные, bss)
    mov ebx, kernel_end
    add ebx, PAGE_SIZE - 1
    shr ebx, PAGE_SHIFT
    dec ebx                         ; последняя занятая страница ядра
    xor eax, eax
    call pmm_mark_range

    ; b) стек ядра: 0x80000..0x90000 (стек растёт вниз от 0x90000)
    mov eax, KERNEL_STACK_TOP - 0x10000
    shr eax, PAGE_SHIFT
    mov ebx, KERNEL_STACK_TOP
    shr ebx, PAGE_SHIFT
    dec ebx
    call pmm_mark_range

    ; c) E820 не всегда описывает первую страницу (там IVT/BDA) —
    ;    на всякий случай резервируем страницу 0 отдельно.
    xor eax, eax
    xor ebx, ebx
    call pmm_mark_range

    ; ---- 4. Посчитать свободные страницы ----
    call pmm_count_free

    pop edi
    pop esi
    pop edx
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

    push pmm_lock
    call spin_lock
    add esp, 4

    xor ecx, ecx

.scan:
    cmp ecx, [page_limit]
    jae .nomem

    push ecx
    mov eax, ecx
    call pmm_test_bit
    pop ecx

    jnc .found                      ; бит 0 — страница свободна

.next:
    inc ecx
    jmp .scan

.found:
    push ecx
    mov eax, ecx
    call pmm_set_bit                ; занять страницу
    pop ecx

    dec dword [free_pages]

    mov eax, ecx
    shl eax, PAGE_SHIFT             ; адрес = индекс * 4096

    push eax
    push pmm_lock
    call spin_unlock
    add esp, 4
    pop eax

    pop ecx
    pop ebx
    ret

.nomem:
    push pmm_lock
    call spin_unlock
    add esp, 4

    xor eax, eax
    pop ecx
    pop ebx
    ret

; ---------------------------------------------------------
; void pmm_free(void *addr)
;
; Освобождает страницу, полученную из pmm_alloc().
; Игнорируются: NULL, невыровненные адреса, адреса вне модели
; памяти, страницы вне доступных областей E820, страницы, которые
; не были выделены, и зарезервированные области (ядро, стек).
; ---------------------------------------------------------
global pmm_free
pmm_free:
    push ebx

    mov ebx, [esp + 8]              ; addr
    test ebx, ebx
    jz .done

    test ebx, PAGE_SIZE - 1         ; должен быть выровнен по странице
    jnz .done

    shr ebx, PAGE_SHIFT             ; индекс страницы
    cmp ebx, [page_limit]
    jae .done

    push edx

    push pmm_lock
    call spin_lock
    add esp, 4

    ; страница должна быть выделена
    mov eax, ebx
    call pmm_test_bit
    jnc .unlock

    ; не освобождаем всё до конца образа ядра
    mov eax, kernel_end
    add eax, PAGE_SIZE - 1
    shr eax, PAGE_SHIFT             ; первая свободная страница за ядром
    cmp ebx, eax
    jb .unlock

    ; не освобождаем стек ядра
    cmp ebx, (KERNEL_STACK_TOP - 0x10000) >> PAGE_SHIFT
    jb .really_free
    cmp ebx, KERNEL_STACK_TOP >> PAGE_SHIFT
    jb .unlock

.really_free:
    mov eax, ebx
    call pmm_clear_bit
    inc dword [free_pages]

.unlock:
    push pmm_lock
    call spin_unlock
    add esp, 4

    pop edx
.done:
    pop ebx
    ret

; ---------------------------------------------------------
; uint32_t pmm_free_pages(void)  — сколько страниц свободно
; uint32_t pmm_total_pages(void) — сколько страниц покрывает bitmap
; uint32_t pmm_usable_bytes(void)— столько байт доступной ОЗУ
; ---------------------------------------------------------
global pmm_free_pages
pmm_free_pages:
    mov eax, [free_pages]
    ret

global pmm_total_pages
pmm_total_pages:
    mov eax, [page_limit]
    ret

global pmm_usable_bytes
pmm_usable_bytes:
    mov eax, [page_limit]
    shl eax, PAGE_SHIFT
    ret

; ---------------------------------------------------------
; uint32_t pmm_e820_entries(void) — записей в карте E820
; uint32_t pmm_e820_usable(void)  — из них доступных (type == 1)
; ---------------------------------------------------------
global pmm_e820_entries
pmm_e820_entries:
    mov eax, [e820_entries]
    ret

global pmm_e820_usable
pmm_e820_usable:
    mov eax, [e820_usable]
    ret

; ---------------------------------------------------------
; int pmm_e820_entry(uint32_t index, e820_entry_t *out)
;   Копирует запись E820 номер index в out.
;   Возвращает 1 при успехе и 0, если такого индекса нет.
; ---------------------------------------------------------
global pmm_e820_entry
pmm_e820_entry:
    push ebx
    push esi
    push edi

    mov eax, [esp + 16]             ; index
    cmp eax, [e820_entries]
    jae .fail

    mov edi, [esp + 20]             ; out
    test edi, edi
    jz .fail

    imul eax, E820_ENTRY_SIZE
    add eax, E820_BUF_ADDR
    mov esi, eax
    mov ecx, E820_ENTRY_SIZE / 4
    rep movsd

    mov eax, 1
    jmp .done
.fail:
    xor eax, eax
.done:
    pop edi
    pop esi
    pop ebx
    ret

section .note.GNU-stack noalloc noexec nowrite progbits
