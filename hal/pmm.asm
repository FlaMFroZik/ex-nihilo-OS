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

    mov al, [bitmap + ebx]
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

    ; установить бит
    mov al, 1
    mov cl, dl
    shl al, cl
    or [bitmap + ebx], al

    ; вернуть физический адрес = index * 4096
    mov eax, ecx
    shl eax, 12

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
    shr ebx, 12

    mov ecx, ebx
    shr ecx, 3
    mov edx, ebx
    and edx, 7

    mov al, 1
    mov cl, dl
    shl al, cl
    not al
    and [bitmap + ecx], al

    ; освободить лок
    push pmm_lock
    call spin_unlock
    add esp, 4

    pop edx
    pop ecx
    pop ebx
    ret
