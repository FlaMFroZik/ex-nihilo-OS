#ifndef SYSCALL_H
#define SYSCALL_H

#include <stdint.h>

/* ================================================================
 * syscall — переход Ring 3 -> Ring 0 и обратно.
 *
 * Как это работает (классика i386, int 0x80):
 *   1. Код Ring 3 кладёт номер syscall'а в EAX, аргументы — в
 *      EBX, ECX, EDX, ESI, EDI и выполняет `int 0x80`.
 *   2. Вектор 0x80 в IDT — interrupt gate с DPL=3 (иначе процессор
 *      не дал бы Ring 3 его дёрнуть: #GP). По прерыванию CPU сам
 *      переключает стек на TSS.ESP0 и сохраняет SS/ESP/EFLAGS/CS/EIP
 *      пользователя — мы оказываемся в Ring 0 (CPL=0).
 *   3. Обработчик (kernel/syscall.asm) сохраняет регистры и зовёт
 *      syscall_dispatch(): тот по номеру вызывает настоящую функцию
 *      ядра, например kopen(path, flags).
 *   4. Результат кладётся обратно в сохранённый EAX, `iret`
 *      восстанавливает CS/EIP/SS/ESP пользователя — функция ядра
 *      вернулась, и syscall тоже возвращается в Ring 3, ровно в
 *      инструкцию после `int 0x80`.
 *
 * SYS_KEXIT — особый случай: возвращаться в Ring 3 некуда (юзерский
 * код закончился), поэтому обработчик восстанавливает сохранённый
 * стек ядра и enter_usermode() возвращает код завершения.
 * ================================================================ */

#define SYS_KEXIT   0
#define SYS_KOPEN   1
#define SYS_KCLOSE  2
#define SYS_KREAD   3
#define SYS_KWRITE  4
#define SYS_KSEEK   5
#define SYS_KUNLINK 6
#define SYS_KPRINT  7
#define SYS_KALLOC  8   /* -> kpmm_alloc() */
#define SYS_KFREE   9   /* -> kpmm_free()  */
#define SYS_KEXECVE 10  /* -> kexecve(): при успехе не возвращается */
#define SYS_MAX     11

/* --- Сторона Ring 0 (вызывает kmain) --- */

/* Своя GDT (kernel/data DPL=0, user code/data DPL=3, TSS), TSS с
 * ESP0 = стек ядра и IDT с вектором 0x80 (DPL=3). Ставить один раз. */
void syscall_cpu_init(void);

/* Перейти в Ring 3 на entry со стеком user_stack_top (iret строит
 * кадр SS=0x23/CS=0x1B). Вернётся, когда Ring 3 сделает SYS_KEXIT:
 * возвращает его код завершения. Отдельный код — USER_EXIT_PAGEFAULT:
 * Ring 3 обратился к чужой памяти, #PF его убил. */
#define USER_EXIT_PAGEFAULT 240
int enter_usermode(void (*entry)(void), void *user_stack_top);

/* Диспетчер: вызывается из asm-обработчика int 0x80. */
int32_t syscall_dispatch(uint32_t nr, uint32_t a1, uint32_t a2,
                         uint32_t a3, uint32_t a4, uint32_t a5);

/* --- Сторона Ring 3: стабы. Использовать ТОЛЬКО из юзерского кода --- */

static inline int32_t syscall5(uint32_t nr, uint32_t a1, uint32_t a2,
                               uint32_t a3, uint32_t a4, uint32_t a5)
{
    int32_t ret;
    __asm__ volatile ("int $0x80"
                      : "=a"(ret)
                      : "a"(nr), "b"(a1), "c"(a2), "d"(a3), "S"(a4), "D"(a5)
                      : "memory");
    return ret;
}

static inline int32_t sys_kopen(const char *path, int flags)
{ return syscall5(SYS_KOPEN, (uint32_t)path, (uint32_t)flags, 0, 0, 0); }

static inline int32_t sys_kclose(int fd)
{ return syscall5(SYS_KCLOSE, (uint32_t)fd, 0, 0, 0, 0); }

static inline int32_t sys_kread(int fd, void *buf, uint32_t count)
{ return syscall5(SYS_KREAD, (uint32_t)fd, (uint32_t)buf, count, 0, 0); }

static inline int32_t sys_kwrite(int fd, const void *buf, uint32_t count)
{ return syscall5(SYS_KWRITE, (uint32_t)fd, (uint32_t)buf, count, 0, 0); }

static inline int32_t sys_kprint(const char *str)
{ return syscall5(SYS_KPRINT, (uint32_t)str, 0, 0, 0, 0); }

static inline int32_t sys_execve(const char *path, char *const argv[],
                                 char *const envp[])
{ return syscall5(SYS_KEXECVE, (uint32_t)path, (uint32_t)argv,
                  (uint32_t)envp, 0, 0); }

static inline void *sys_kalloc(void)
{ return (void *)syscall5(SYS_KALLOC, 0, 0, 0, 0, 0); }

static inline void sys_kfree(void *addr)
{ (void)syscall5(SYS_KFREE, (uint32_t)addr, 0, 0, 0, 0); }

static inline void sys_kexit(int code)
{
    (void)syscall5(SYS_KEXIT, (uint32_t)code, 0, 0, 0, 0);
    for (;;) { }    /* сюда управление не возвращается */
}

#endif
