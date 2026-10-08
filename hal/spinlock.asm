; hal/spinlock.asm
; Простой спинлок на xchg

section .text

; ---------------------------------------------------------
; void spin_lock(uint32_t *lock)
; ---------------------------------------------------------
global spin_lock
spin_lock:
    mov ecx, [esp + 4]      ; lock
    mov eax, 1
.retry:
    xchg eax, [ecx]         ; атомарный обмен
    test eax, eax
    jnz .retry              ; если был 1 — снова
    ret

; ---------------------------------------------------------
; void spin_unlock(uint32_t *lock)
; ---------------------------------------------------------
global spin_unlock
spin_unlock:
    mov ecx, [esp + 4]      ; lock
    mov dword [ecx], 0      ; освободить
    ret

; ---------------------------------------------------------
; void spin_lock_irqsave(uint32_t *lock, uint32_t *flags)
; ---------------------------------------------------------
global spin_lock_irqsave
spin_lock_irqsave:
    pushfd
    pop eax
    mov edx, [esp + 8]      ; flags
    mov [edx], eax          ; сохранить флаги
    cli                     ; отключить прерывания
    mov ecx, [esp + 4]      ; lock
    mov eax, 1
.retry:
    xchg eax, [ecx]
    test eax, eax
    jnz .retry
    ret

; ---------------------------------------------------------
; void spin_unlock_irqrestore(uint32_t *lock, uint32_t flags)
; ---------------------------------------------------------
global spin_unlock_irqrestore
spin_unlock_irqrestore:
    mov ecx, [esp + 4]      ; lock
    mov dword [ecx], 0
    mov eax, [esp + 8]      ; flags
    push eax
    popfd                   ; восстановить флаги
    ret
