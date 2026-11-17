[BITS 32]

; ================================================================
; syscall.asm — железо перехода Ring 3 <-> Ring 0:
;   * своя GDT: kernel code/data (DPL=0), user code/data (DPL=3), TSS;
;   * TSS: по прерыванию из Ring 3 процессор берёт стек Ring 0 из
;     TSS.ESP0/SS0 — без TSS int из Ring 3 не переключил бы стек;
;   * IDT: вектор 0x80 — interrupt gate с DPL=3 (только такой gate
;     Ring 3 вправе дёрнуть через `int`), остальные векторы пустые;
;   * обработчик int 0x80: сохраняет регистры Ring 3, зовёт
;     syscall_dispatch() (C, Ring 0), результат — в сохранённый EAX,
;     `iret` возвращает управление Ring 3 после `int 0x80`;
;   * enter_usermode(): строит кадр iret (SS/ESP/EFLAGS/CS/EIP)
;     и "возвращается" сразу в Ring 3.
; ================================================================

KERNEL_CS equ 0x08
KERNEL_DS equ 0x10
USER_CS   equ 0x1B              ; 0x18 | RPL3
USER_DS   equ 0x23              ; 0x20 | RPL3
TSS_SEL   equ 0x28
SYS_KEXIT equ 0

RING0_STACK_TOP equ 0x8F000     ; внутри зарезервированного под стек
                                ; диапазона 0x80000..0x90000 (см. pmm)

global syscall_cpu_init
global enter_usermode
global exec_jump
extern syscall_dispatch

section .text

; ---------------------------------------------------------------
; syscall_cpu_init(void)
; ---------------------------------------------------------------
syscall_cpu_init:
    ; --- TSS: обнулить, ESP0/SS0 = стек Ring 0 ---
    mov edi, tss
    mov ecx, 26                 ; 104 байта
    xor eax, eax
    rep stosd
    mov dword [tss + 4], RING0_STACK_TOP    ; ESP0
    mov dword [tss + 8], KERNEL_DS          ; SS0
    mov dword [tss + 100], 104              ; I/O bitmap offset = за TSS

    ; --- дескриптор TSS в GDT (база = адрес tss) ---
    mov eax, tss
    mov word [gdt_tss + 0], 103             ; лимит
    mov word [gdt_tss + 2], ax
    shr eax, 16
    mov byte [gdt_tss + 4], al
    mov byte [gdt_tss + 5], 0x89            ; present, DPL=0, TSS(386) available
    mov byte [gdt_tss + 6], 0
    mov byte [gdt_tss + 7], ah

    ; --- IDT: обнулить, вектор 0x80 -> syscall_entry, gate DPL=3 ---
    mov edi, idt
    mov ecx, 256 * 2                        ; 256 записей по 8 байт
    xor eax, eax
    rep stosd

    mov eax, syscall_entry
    mov word [idt + 0x80 * 8 + 0], ax
    mov word [idt + 0x80 * 8 + 2], KERNEL_CS
    mov byte [idt + 0x80 * 8 + 4], 0
    mov byte [idt + 0x80 * 8 + 5], 0xEE     ; present, DPL=3, 32-bit int gate
    shr eax, 16
    mov word [idt + 0x80 * 8 + 6], ax

    lgdt [gdt_descriptor]
    jmp KERNEL_CS:.reload_cs
.reload_cs:
    mov ax, KERNEL_DS
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax

    mov ax, TSS_SEL
    ltr ax
    lidt [idt_descriptor]
    ret

; ---------------------------------------------------------------
; обработчик int 0x80. Сюда попадаем уже в Ring 0, на стеке
; (от TSS.ESP0) лежат SS/ESP/EFLAGS/CS/EIP пользователя.
; EAX = номер, EBX..EDI = аргументы (см. syscall.h).
; ---------------------------------------------------------------
syscall_entry:
    cmp eax, SYS_KEXIT
    je .exit

    ; СНАЧАЛА pushad: номер syscall'а лежит в EAX, а загрузка
    ; сегментов ниже делается через AX и номер бы затёрла.
    pushad                      ; слоты от ESP: EDI,ESI,EBP,ESP,EBX,EDX,ECX,EAX
    push ds
    push es
    push fs
    push gs
    mov ax, KERNEL_DS
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax

    ; Аргументы dispatch'а берём из слотов pushad (от текущего ESP:
    ; EDI=+16, ESI=+20, EBX=+32, EDX=+36, ECX=+40, EAX=+44; каждый
    ; push сдвигает следующие смещения на 4).
    ; syscall_dispatch(nr=EAX, a1=EBX, a2=ECX, a3=EDX, a4=ESI, a5=EDI)
    push dword [esp + 16]       ; a5 = EDI
    push dword [esp + 24]       ; a4 = ESI
    push dword [esp + 44]       ; a3 = EDX
    push dword [esp + 52]       ; a2 = ECX
    push dword [esp + 48]       ; a1 = EBX
    push dword [esp + 64]       ; nr = EAX
    call syscall_dispatch
    add esp, 24

    mov [esp + 44], eax         ; результат -> сохранённый EAX pushad'а

    pop gs
    pop fs
    pop es
    pop ds
    popad
    iret                        ; Ring 3, инструкция после int 0x80

.exit:
    ; SYS_KEXIT: в Ring 3 не возвращаемся. Восстанавливаем стек,
    ; сохранённый enter_usermode(), и делаем обычный ret из него.
    mov eax, ebx                ; код завершения от пользователя
    mov [user_exit_code], eax
    mov esp, [saved_kernel_esp]
    mov ax, KERNEL_DS
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    pop ebx                     ; callee-saved, сохранённые enter_usermode
    pop esi
    pop edi
    pop ebp
    mov eax, [user_exit_code]
    ret

; ---------------------------------------------------------------
; void exec_jump(uint32_t entry, uint32_t user_esp) — не возвращается.
;
; Вызывается из kexecve() уже в Ring 0 (мы внутри обработчика
; int 0x80, на стеке Ring 0). Старый кадр syscall'а просто бросаем:
; образ Ring 3 заменён, возвращаться в него некуда. Строим такой же
; кадр iret, как enter_usermode, но сегменты данных сразу ставим
; пользовательские: у загруженного ELF нет кода, который бы их
; перезагрузил, а лежать в регистрах после iret останутся именно они.
; saved_kernel_esp от enter_usermode не трогаем — по SYS_KEXIT из
; нового образа управление вернётся в kmain, как обычно.
; ---------------------------------------------------------------
exec_jump:
    mov eax, [esp + 4]              ; entry
    mov ecx, [esp + 8]              ; user_esp (argc/argv/envp уже там)

    mov dx, USER_DS
    mov ds, dx
    mov es, dx
    mov fs, dx
    mov gs, dx

    push USER_DS                    ; SS
    push ecx                        ; ESP (стек новой программы)
    push dword 0x002                ; EFLAGS: IF=0
    push USER_CS                    ; CS
    push eax                        ; EIP = e_entry
    iret                            ; Ring 3, новый образ

; ---------------------------------------------------------------
; int enter_usermode(void (*entry)(void), void *user_stack_top)
; ---------------------------------------------------------------
enter_usermode:
    ; Callee-saved регистры kmain обязаны пережить вояж в Ring 3,
    ; а SYS_KEXIT минует эпилог юзерского кода — сохраняем сами.
    push ebp
    push edi
    push esi
    push ebx
    mov [saved_kernel_esp], esp
    mov eax, [esp + 20]             ; entry (после 4 push'ей)
    mov ecx, [esp + 24]             ; user_stack_top

    push USER_DS                    ; SS
    push ecx                        ; ESP (стек Ring 3)
    push dword 0x002                ; EFLAGS: IF=0 (прерывания не настроены)
    push USER_CS                    ; CS
    push eax                        ; EIP
    iret                            ; CPL 3, увидимся на SYS_KEXIT

section .data
align 8
gdt_start:
    dq 0                                        ; null
    dq 0x00CF9A000000FFFF                       ; kernel code, DPL=0
    dq 0x00CF92000000FFFF                       ; kernel data, DPL=0
    dq 0x00CFFA000000FFFF                       ; user code,   DPL=3
    dq 0x00CFF2000000FFFF                       ; user data,   DPL=3
gdt_tss:
    dq 0                                        ; TSS, заполняется в init
gdt_end:

gdt_descriptor:
    dw gdt_end - gdt_start - 1
    dd gdt_start

idt_descriptor:
    dw 256 * 8 - 1
    dd idt

section .bss
align 8
tss:                 resb 104
idt:                 resb 256 * 8
saved_kernel_esp:    resd 1
user_exit_code:      resd 1

section .note.GNU-stack noalloc noexec nowrite progbits
