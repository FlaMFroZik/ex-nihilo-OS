; hal/spinlock.asm
; Простой спинлок на xchg (32-bit x86)
;
; Соглашение о вызовах cdecl: eax/ecx/edx можно менять.

section .text

; ---------------------------------------------------------
; void spin_lock(uint32_t *lock)
; ---------------------------------------------------------
global spin_lock
spin_lock:
    mov ecx, [esp + 4]          ; lock
.acquire:
    mov eax, 1
    xchg eax, [ecx]             ; атомарно: lock = 1, eax = прежнее значение
    test eax, eax
    jz .acquired                ; было 0 — лок наш
.spin:
    pause                       ; подсказка процессору (Hyper-Threading)
    cmp dword [ecx], 0
    je .acquire                 ; лок освободился — пробуем снова
    jmp .spin
.acquired:
    ret

; ---------------------------------------------------------
; void spin_unlock(uint32_t *lock)
; ---------------------------------------------------------
global spin_unlock
spin_unlock:
    mov ecx, [esp + 4]          ; lock
    mov dword [ecx], 0          ; освободить
    ret

; ---------------------------------------------------------
; void spin_lock_irqsave(uint32_t *lock, uint32_t *flags)
;
; Порядок важен: сначала cli, потом захват лока. Иначе между
; захватом лока и cli может прийти прерывание, обработчик
; попробует взять тот же лок и получится дедлок.
; ---------------------------------------------------------
global spin_lock_irqsave
spin_lock_irqsave:
    push ebx
    pushfd
    pop eax
    mov ebx, [esp + 12]         ; flags (после push ebx смещение сдвинулось)
    mov [ebx], eax              ; сохранить исходный RFLAGS
    cli
    mov ecx, [esp + 8]          ; lock
.acquire:
    mov eax, 1
    xchg eax, [ecx]
    test eax, eax
    jz .acquired
.spin:
    pause
    cmp dword [ecx], 0
    je .acquire
    jmp .spin
.acquired:
    pop ebx
    ret

; ---------------------------------------------------------
; void spin_unlock_irqrestore(uint32_t *lock, uint32_t flags)
;
; Лок отпускаем ДО восстановления флагов: иначе между popfd
; и освобождением лока другой CPU увидит занятый лок, а
; прерывания уже включены — лишняя задержка, а на UP-системе
; просто неверный порядок.
; ---------------------------------------------------------
global spin_unlock_irqrestore
spin_unlock_irqrestore:
    mov ecx, [esp + 4]          ; lock
    mov dword [ecx], 0          ; сначала отпустить лок
    mov eax, [esp + 8]          ; flags
    push eax
    popfd                       ; восстановить RFLAGS (в т.ч. IF)
    ret

section .note.GNU-stack noalloc noexec nowrite progbits
