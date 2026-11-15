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
;   1. Весь bitmap помечается занятым (fail closed).
;   2. Освобождаются только области type == 1 (доступные ОС).
;   3. Затем ЗАНОВО помечаются занятыми все области type != 1.
;      Порядок важен: записи E820 могут пересекаться (классика —
;      базовая память кончается на 0x9FC00, а EBDA начинается там
;      же, и одна страница принадлежит обеим записям), поэтому
;      «занято» должно выигрывать у «свободно».
;   4. Резервируются ядро, стек ядра и страница 0.
;
; Сам bitmap живёт в .bss ядра (kernel_end уже зарезервирован),
; то есть занимать под него страницы у самого себя не нужно.
; ============================================================

%define PAGE_SHIFT      12
%define PAGE_SIZE       4096

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

; 1 бит на страницу: 128 КиБ bitmap покрывает 4 ГиБ адресного пространства.
%define BITMAP_MAX_BYTES (1024 * 1024 / 8)          ; 128 КиБ => 4 ГиБ памяти
%define PAGE_LIMIT_MAX   (BITMAP_MAX_BYTES * 8)     ; 1048576 страниц

%define KERNEL_STACK_TOP 0x90000                    ; см. kernel.asm
%define ALLOC_HINT_WRAPPED 0xFFFFFFFF               ; «поиск уже обернулся»
%define STACK_BOTTOM     (KERNEL_STACK_TOP - 0x10000)

; Всё, что выше 4 ГиБ, нам недоступно: адреса 32-битные.
; LAST_ADDR — последний байт, который ещё адресуется 32-битным
; регистром. Именно 0xFFFFFFFF, а не 0xFFFFF000: иначе последняя
; страница диапазона (0xFFFFF000) выпадала бы из модели, и при
; ровно 4 ГиБ ОЗУ page_limit получался на 1 меньше — pmm_usable_bytes()
; после умножения на 4096 давал 0.
%define LAST_ADDR        0xFFFFFFFF

; Легаси-области, которые нельзя отдавать, если карты E820 нет.
; Границы заданы в НОМЕРАХ СТРАНИЦ: страница, на которую область
; только начинается, тоже занята целиком.
%define EBDA_PAGE_FIRST  (0x0009FC00 / PAGE_SIZE)   ; 0x9F — EBDA (0x9FC00)
%define EBDA_PAGE_LAST   (0x0009FFFF / PAGE_SIZE)   ; 0x9F
%define VGA_PAGE_FIRST   (0x000A0000 / PAGE_SIZE)   ; 0xA0 — VGA (0xA0000)
%define VGA_PAGE_LAST    (0x000BFFFF / PAGE_SIZE)   ; 0xBF
%define ROM_PAGE_FIRST   (0x000F0000 / PAGE_SIZE)   ; 0xF0 — BIOS ROM
%define ROM_PAGE_LAST    (0x000FFFFF / PAGE_SIZE)   ; 0xFF

section .bss
    alignb 4
    bitmap          resb BITMAP_MAX_BYTES   ; 1 бит = 1 страница
    pmm_lock        resd 1
    page_limit      resd 1                  ; сколько страниц покрывает bitmap
    free_pages      resd 1                  ; свободных страниц
    have_e820       resd 1                  ; 1 — модель построена по карте BIOS
    alloc_hint      resd 1                  ; с какой страницы начинать поиск
    e820_entries    resd 1                  ; сколько записей реально прочитано
    e820_usable     resd 1                  ; сколько из них дали память
    usable_pages    resd 1                  ; страниц по 4 КиБ в областях type == 1

section .text
extern spin_lock
extern spin_unlock
extern kernel_end

; ---------------------------------------------------------
; pmm_set_bit / pmm_clear_bit / pmm_test_bit
;   eax = номер страницы
;   Границы проверяет вызывающий.
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
; pmm_entry_bounds
;   вход:  edi -> запись E820
;   выход: eax = начало области (байты), edx = конец (не включая),
;          CF = 1, если запись пригодна для 32-битной модели;
;          CF = 0, если запись начинается за пределами 4 ГиБ.
;   Области, выходящие за 4 ГиБ, обрезаются по LAST_ADDR.
; ---------------------------------------------------------
pmm_entry_bounds:
    cmp dword [edi + 4], 0          ; старшее слово base
    jne .beyond
    mov eax, [edi]                  ; base
    mov edx, eax
    cmp dword [edi + 12], 0         ; старшее слово length
    jne .clamp
    add edx, [edi + 8]              ; end = base + length
    jnc .done
.clamp:
    mov edx, LAST_ADDR
    jmp .done
.beyond:
    xor eax, eax
    xor edx, edx
    clc
    ret
.done:
    cmp edx, LAST_ADDR
    jbe .ok
    mov edx, LAST_ADDR
.ok:
    stc
    ret

; ---------------------------------------------------------
; pmm_count_entries — сколько записей E820 читаем (не больше E820_MAX)
;   выход: esi
; ---------------------------------------------------------
pmm_count_entries:
    mov esi, [E820_COUNT_ADDR]
    cmp esi, E820_MAX
    jbe .ok
    mov esi, E820_MAX
.ok:
    ret

; ---------------------------------------------------------
; pmm_clear_usable_entries — освободить все области type == 1
;   Заодно растягивает page_limit.
; ---------------------------------------------------------
pmm_clear_usable_entries:
    pushad

    call pmm_count_entries
    mov edi, E820_BUF_ADDR

.loop:
    test esi, esi
    jz .done
    dec esi

    cmp dword [edi + 16], E820_TYPE_FREE
    jne .next

    call pmm_entry_bounds
    jnc .next
    cmp edx, eax
    jbe .next                       ; пустая область

    ; сколько страниц до конца области
    mov ecx, edx
    dec ecx
    shr ecx, PAGE_SHIFT
    inc ecx
    cmp ecx, PAGE_LIMIT_MAX
    jbe .limit_ok
    mov ecx, PAGE_LIMIT_MAX
.limit_ok:
    cmp ecx, [page_limit]
    jbe .limit_kept
    mov [page_limit], ecx
.limit_kept:

    inc dword [e820_usable]

    shr eax, PAGE_SHIFT             ; первая страница области

    ; учесть, сколько страниц по 4 КиБ принесла эта область
    mov ebx, ecx
    sub ebx, eax
    add [usable_pages], ebx

.free_loop:
    btr [bitmap], eax
    inc eax
    cmp eax, ecx
    jb .free_loop

.next:
    add edi, E820_ENTRY_SIZE
    jmp .loop
.done:
    popad
    ret

; ---------------------------------------------------------
; pmm_reserve_unusable_entries — пометить занятыми области type != 1
;
; Выполняется ПОСЛЕ pmm_clear_usable_entries: если записи пересекаются,
; «занято» должно победить. Иначе страница EBDA (0x9F000), попавшая
; сразу в «базовую память» и в «EBDA», осталась бы свободной.
; ---------------------------------------------------------
pmm_reserve_unusable_entries:
    pushad

    call pmm_count_entries
    mov edi, E820_BUF_ADDR

.loop:
    test esi, esi
    jz .done
    dec esi

    cmp dword [edi + 16], E820_TYPE_FREE
    je .next                        ; это свободная область

    call pmm_entry_bounds
    jnc .next
    cmp edx, eax
    jbe .next

    ; отметить страницы [base/4096, (end-1)/4096]
    push edx
    mov ebx, edx
    dec ebx
    shr ebx, PAGE_SHIFT
    shr eax, PAGE_SHIFT
    call pmm_mark_range
    pop edx

.next:
    add edi, E820_ENTRY_SIZE
    jmp .loop
.done:
    popad
    ret

; ---------------------------------------------------------
; pmm_mark_range
;   вход: eax = первая страница, ebx = последняя (включительно)
;   Без лока: вызывается только из pmm_init.
; ---------------------------------------------------------
pmm_mark_range:
    push eax
    push ebx

    cmp eax, ebx
    ja .done
    cmp eax, [page_limit]
    jae .done                       ; диапазон целиком за пределом модели

    cmp ebx, [page_limit]
    jb .clamped
    mov ebx, [page_limit]
    dec ebx                         ; не выходим за bitmap
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
; ---------------------------------------------------------
pmm_count_free:
    push eax
    push ecx
    push edx

    xor ecx, ecx
    xor eax, eax
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
    pop edx
    pop ecx
    pop eax
    ret

; ---------------------------------------------------------
; pmm_is_usable_page
;   вход:  eax = номер страницы
;   выход: CF = 1, если страницу можно отдавать/освобождать:
;          она целиком лежит в области type == 1 и НЕ пересекается
;          ни с одной областью type != 1.
;
;   Без этой проверки pmm_free() мог «освободить» видеопамять,
;   BIOS ROM или MMIO-дыру: они тоже помечены битом «занято»,
;   как и страницы, выданные аллокатором.
; ---------------------------------------------------------
pmm_is_usable_page:
    pushad

    mov ebx, eax
    shl ebx, PAGE_SHIFT             ; начало страницы
    lea ecx, [ebx + PAGE_SIZE]      ; конец страницы (не включая)

    ; ---- 1. страница должна целиком лежать в области type == 1 ----
    call pmm_count_entries
    mov edi, E820_BUF_ADDR
    xor ebp, ebp                    ; 1 = нашли покрывающую область
.find_free:
    test esi, esi
    jz .find_done
    dec esi

    cmp dword [edi + 16], E820_TYPE_FREE
    jne .find_next

    ; pmm_entry_bounds возвращает начало в eax и КОНЕЦ в edx —
    ; edx нельзя использовать под флаг, иначе теряется конец области
    call pmm_entry_bounds
    jnc .find_next

    cmp ebx, eax                    ; начало страницы >= начала области?
    jb .find_next
    cmp ecx, edx                    ; конец страницы <= конца области?
    ja .find_next

    mov ebp, 1
    jmp .find_done
.find_next:
    add edi, E820_ENTRY_SIZE
    jmp .find_free
.find_done:
    test ebp, ebp
    jz .deny

    ; ---- 2. страница не должна пересекаться с областями type != 1 ----
    call pmm_count_entries
    mov edi, E820_BUF_ADDR
.check_reserved:
    test esi, esi
    jz .allow
    dec esi

    cmp dword [edi + 16], E820_TYPE_FREE
    je .check_next

    call pmm_entry_bounds
    jnc .check_next

    ; пересечение: start < page_end И end > page_start
    cmp edx, ebx
    jbe .check_next
    cmp eax, ecx
    jae .check_next
    jmp .deny
.check_next:
    add edi, E820_ENTRY_SIZE
    jmp .check_reserved

.allow:
    stc
    jmp .done
.deny:
    clc
.done:
    popad
    ret

; ---------------------------------------------------------
; pmm_is_legacy_reserved
;   вход:  eax = номер страницы
;   выход: CF = 1, если страница попадает в EBDA / VGA / BIOS ROM.
;   Используется только когда карты E820 нет.
;
; Страница считается занятой, если она ПЕРЕСЕКАЕТСЯ с областью:
; EBDA начинается на 0x9FC00, то есть внутри страницы 0x9F, поэтому
; сравнивать надо с началом и концом страницы, а не самой области.
; ---------------------------------------------------------
pmm_is_legacy_reserved:
    pushad

    mov eax, [esp + 32]             ; номер страницы (pushad = 32 байта)
    mov ebx, eax
    shl ebx, PAGE_SHIFT             ; начало страницы
    lea ecx, [ebx + PAGE_SIZE]      ; конец страницы (не включая)

    ; --- EBDA: 0x9FC00 .. 0xA0000 ---
    mov eax, 0x0009FC00
    mov edx, 0x000A0000
    call .overlaps

    ; --- видеопамять: 0xA0000 .. 0xC0000 ---
    mov eax, 0x000A0000
    mov edx, 0x000C0000
    call .overlaps

    ; --- BIOS ROM: 0xF0000 .. 0x100000 ---
    mov eax, 0x000F0000
    mov edx, 0x00100000
    call .overlaps

    clc
    jmp .done

; eax = начало области, edx = конец области, ebx/ecx = страница.
; CF = 1, если [ebx, ecx) пересекается с [eax, edx).
.overlaps:
    cmp edx, ebx                    ; область кончается до страницы?
    jbe .no
    cmp eax, ecx                    ; область начинается после страницы?
    jae .no
    stc
    ret
.no:
    clc
    ret

.done:
    popad
    ret

; ---------------------------------------------------------
; void pmm_init(void)
;
; Строит bitmap по карте E820 из загрузчика.
; ---------------------------------------------------------
global pmm_init
pmm_init:
    pushad

    mov dword [pmm_lock], 0
    mov dword [page_limit], 0
    mov dword [free_pages], 0
    mov dword [e820_usable], 0
    mov dword [usable_pages], 0
    mov dword [have_e820], 0
    mov dword [alloc_hint], 0

    ; ---- 1. Всё занято: пока карта не разобрана, память не выдаём ----
    mov edi, bitmap
    mov ecx, BITMAP_MAX_BYTES / 4
    mov eax, 0xFFFFFFFF
    rep stosd

    ; ---- 2. Разобрать карту E820 ----
    call pmm_count_entries
    mov [e820_entries], esi
    test esi, esi
    jz .no_e820                     ; загрузчик не смог опросить BIOS

    mov dword [have_e820], 1
    call pmm_clear_usable_entries   ; type == 1 -> свободно
    call pmm_reserve_unusable_entries ; type != 1 -> занято (выигрывает)
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
    cmp eax, LAST_ADDR
    jbe .low_ok
    mov eax, LAST_ADDR
.low_ok:
    mov ebx, eax
    dec ebx
    shr ebx, PAGE_SHIFT
    inc ebx
    mov [page_limit], ebx
    xor eax, eax
    call pmm_clear_range

    ; без карты E820 сами резервируем EBDA, видеопамять и BIOS ROM
    mov ebx, EBDA_PAGE_LAST
    mov eax, EBDA_PAGE_FIRST
    call pmm_mark_range

    mov ebx, VGA_PAGE_LAST
    mov eax, VGA_PAGE_FIRST
    call pmm_mark_range

    mov ebx, ROM_PAGE_LAST
    mov eax, ROM_PAGE_FIRST
    call pmm_mark_range

.after_map:

    ; ---- 3. Резервируем то, что отдавать нельзя ----
    ; a) всё до конца образа ядра (загрузчик, код, данные, bss)
    mov ebx, kernel_end
    add ebx, PAGE_SIZE - 1
    shr ebx, PAGE_SHIFT
    dec ebx                         ; последняя занятая страница ядра
    xor eax, eax
    call pmm_mark_range

    ; b) стек ядра (растёт вниз от KERNEL_STACK_TOP)
    mov eax, STACK_BOTTOM >> PAGE_SHIFT
    mov ebx, (KERNEL_STACK_TOP >> PAGE_SHIFT) - 1
    call pmm_mark_range

    ; c) страница 0 (IVT/BDA)
    xor eax, eax
    xor ebx, ebx
    call pmm_mark_range

    ; ---- 4. Посчитать свободные страницы ----
    call pmm_count_free

    popad
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

    ; Начинаем с подсказки: без неё каждая выдача просматривала bitmap
    ; с нуля, и N выделений подряд стоили O(N^2).
    mov ecx, [alloc_hint]
    cmp ecx, [page_limit]
    jb .scan
    xor ecx, ecx

.scan:
    cmp ecx, [page_limit]
    jae .wrap

    ; бит читаем напрямую из bitmap: вызов pmm_test_bit на каждую
    ; страницу превращал поиск миллиона страниц в миллион call/ret
    mov eax, ecx
    mov ebx, ecx
    shr eax, 3                      ; номер байта bitmap
    and ebx, 7                      ; номер бита
    movzx edx, byte [bitmap + eax]
    bt edx, ebx
    jnc .found                      ; бит 0 — страница свободна

    inc ecx
    jmp .scan

.wrap:
    ; дошли до page_limit: если поиск начался с подсказки, часть
    ; bitmap в начале могла быть пропущена — проверяем её РОВНО ОДИН раз.
    ; Маркер «уже обернулись» — ALLOC_HINT_WRAPPED: без него при
    ; полностью занятой памяти поиск бесконечно ходил по кругу.
    cmp dword [alloc_hint], ALLOC_HINT_WRAPPED
    je .nomem
    mov dword [alloc_hint], ALLOC_HINT_WRAPPED
    xor ecx, ecx
    jmp .scan

.found:
    ; занять страницу (bts с регистровым номером бита)
    mov eax, ecx
    bts [bitmap], eax

    dec dword [free_pages]
    lea edx, [ecx + 1]
    mov [alloc_hint], edx           ; следующая выдача начнётся отсюда

    mov eax, ecx
    shl eax, PAGE_SHIFT             ; адрес = индекс * 4096

    push eax
    push pmm_lock
    call spin_unlock
    add esp, 4
    pop eax

    pop edx
    pop ecx
    pop ebx
    ret

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
; Освобождает страницу, полученную из pmm_alloc().
; Игнорируются: NULL, невыровненные адреса, адреса вне модели
; памяти, страницы вне доступных областей E820 (видеопамять,
; BIOS ROM, MMIO-дыры), страницы, которые не были выделены,
; и зарезервированные области (ядро, стек).
; ---------------------------------------------------------
global pmm_free
pmm_free:
    push ebx
    push edx

    mov ebx, [esp + 12]             ; addr: 4 (ret) + 4 (ebx) + 4 (edx)
    test ebx, ebx
    jz .done

    test ebx, PAGE_SIZE - 1         ; должен быть выровнен по странице
    jnz .done

    shr ebx, PAGE_SHIFT             ; индекс страницы
    cmp ebx, [page_limit]
    jae .done

    ; не освобождаем всё до конца образа ядра
    mov eax, kernel_end
    add eax, PAGE_SIZE - 1
    shr eax, PAGE_SHIFT             ; первая свободная страница за ядром
    cmp ebx, eax
    jb .done

    ; не освобождаем стек ядра
    cmp ebx, STACK_BOTTOM >> PAGE_SHIFT
    jb .stack_ok
    cmp ebx, (KERNEL_STACK_TOP >> PAGE_SHIFT) - 1
    jbe .done
.stack_ok:

    push pmm_lock
    call spin_lock
    add esp, 4

    ; страница должна быть выделена
    mov eax, ebx
    call pmm_test_bit
    jnc .unlock

    ; страница должна принадлежать доступной памяти, иначе
    ; «освобождение» вернёт в оборот видеопамять или MMIO
    mov eax, ebx
    cmp dword [have_e820], 0
    je .legacy_check
    call pmm_is_usable_page
    jnc .unlock
    jmp .really_free
.legacy_check:
    call pmm_is_legacy_reserved
    jc .unlock

.really_free:
    mov eax, ebx
    call pmm_clear_bit
    inc dword [free_pages]

    ; освобождённая страница должна быть видна следующему поиску
    cmp ebx, [alloc_hint]
    jae .hint_ok
    mov [alloc_hint], ebx
.hint_ok:

.unlock:
    push pmm_lock
    call spin_unlock
    add esp, 4

.done:
    pop edx
    pop ebx
    ret

; ---------------------------------------------------------
; uint32_t pmm_free_pages(void)  — сколько страниц свободно
; uint32_t pmm_total_pages(void) — сколько страниц покрывает bitmap
; uint32_t pmm_usable_bytes(void)— столько байт доступной ОЗУ
;   (значение насыщается: 4 ГиБ не влезают в 32 бита и дают
;    0xFFFFFFFF вместо нуля после переполнения)
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
    mov edx, [page_limit]
    test edx, edx
    jz .zero
    ; page_limit * 4096 переполняет 32 бита при page_limit == 1048576
    cmp edx, 0x00100000             ; 2^20 страниц = 4 ГиБ
    jae .saturate
    mov eax, edx
    shl eax, PAGE_SHIFT
    ret
.saturate:
    mov eax, 0xFFFFFFFF
    ret
.zero:
    xor eax, eax
    ret

; ---------------------------------------------------------
; uint32_t pmm_e820_entries(void) — записей в карте E820
; uint32_t pmm_e820_usable(void)  — сколько записей дали память
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
; uint32_t pmm_usable_pages(void)
;   Сколько страниц по 4 КиБ объявлено доступными (type == 1)
;   в карте E820 — до вычета ядра, стека и прочих резервов.
; ---------------------------------------------------------
global pmm_usable_pages
pmm_usable_pages:
    mov eax, [usable_pages]
    ret

; ---------------------------------------------------------
; uint32_t pmm_kernel_pages(void)
;   Сколько страниц по 4 КиБ занимает образ ядра (включая загрузчик
;   и всё до kernel_end) — они всегда заняты.
; ---------------------------------------------------------
global pmm_kernel_pages
pmm_kernel_pages:
    mov eax, kernel_end
    add eax, PAGE_SIZE - 1
    shr eax, PAGE_SHIFT
    ret

; ---------------------------------------------------------
; int pmm_e820_entry(uint32_t index, e820_entry_t *out)
;   Копирует запись E820 номер index в out.
;   Возвращает 1 при успехе и 0, если такого индекса нет.
; ---------------------------------------------------------
global pmm_e820_entry
pmm_e820_entry:
    push ebx
    push ecx
    push esi
    push edi

    mov eax, [esp + 20]             ; index
    cmp eax, [e820_entries]
    jae .fail

    mov edi, [esp + 24]             ; out
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
    pop ecx
    pop ebx
    ret

section .note.GNU-stack noalloc noexec nowrite progbits
