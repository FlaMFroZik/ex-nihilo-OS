; hal/pmm.asm — PMM со спинлоком
; NASM, 32-bit

section .bss
    bitmap      resb 128        ; 1024 страницы = 4 МБ
    page_count  resd 1
    pmm_lock    resd 1          ; спинлок

section .text
extern spin_lock
extern spin_unlock

; ---------------------------------------------------------
; void pmm_init(void)
; ---------------------------------------------------------
global pmm_init
pmm_init:
    push edi
    push ecx
    push eax

    mov dword [page_count], 1024
    mov dword [pmm_lock], 0     ; лок свободен

    ; очистить bitmap
    mov edi, bitmap
    mov ecx, 128
    xor eax, eax
    rep stosb

    ; пометить первые 16 страниц (0..15) как занятые
    mov dword [bitmap], 0x0000FFFF

    pop eax
    pop ecx
    pop edi
    ret

; ---------------------------------------------------------
; void *pmm_alloc(void)
; ---------------------------------------------------------
global pmm_alloc
pmm_alloc:
    push ebx
    push ecx
    push edx

    ; захватить лок
    push pmm_lock
    call spin_lock
    add esp, 4

    xor ebx, ebx

.scan_byte:
    cmp ebx, 128
    je .nomem

    ; movzx, а не mov al: bt ниже проверяет бит всего eax,
    ; а старшие 3 байта после `mov al` остались бы мусором
    movzx eax, byte [bitmap + ebx]
    cmp al, 0xFF
    jne .found_byte

    inc ebx
    jmp .scan_byte

.found_byte:
    xor edx, edx
.scan_bit:
    bt eax, edx
    jnc .found_bit
    inc edx
    cmp edx, 8
    jl .scan_bit
    inc ebx
    jmp .scan_byte

.found_bit:
    ; индекс = ebx * 8 + edx
    mov ecx, ebx
    shl ecx, 3
    add ecx, edx

    ; вернуть физический адрес = index * 4096.
    ; Считаем его СРАЗУ: ниже понадобится cl для сдвига, а cl — это
    ; младший байт ecx, поэтому ecx к тому моменту будет испорчен.
    mov eax, ecx
    shl eax, 12

    ; установить бит: маска 1 << (index & 7)
    mov cl, dl
    mov edx, 1
    shl edx, cl
    or [bitmap + ebx], dl

    ; освободить лок
    push pmm_lock
    call spin_unlock
    add esp, 4

    pop edx
    pop ecx
    pop ebx
    ret

.nomem:
    ; освободить лок
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
; ---------------------------------------------------------
global pmm_free
pmm_free:
    push ebx
    push ecx
    push edx

    ; захватить лок
    push pmm_lock
    call spin_lock
    add esp, 4

    mov ebx, [esp + 16]         ; addr
    shr ebx, 12                 ; индекс страницы

    ; Сбросить бит (btr = bit test and reset): сама инструкция находит
    ; байт bitmap[index/8] и бит index%8. Раньше здесь было `mov cl, dl`,
    ; из-за чего ecx становился номером БИТА вместо номера БАЙТА
    ; и чистился совсем не тот байт.
    btr [bitmap], ebx

    ; освободить лок
    push pmm_lock
    call spin_unlock
    add esp, 4

    pop edx
    pop ecx
    pop ebx
    ret
