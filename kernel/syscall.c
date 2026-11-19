#include "kernel/syscall.h"
#include "kernel/kapi.h"
#include "hal/video.h"

/* Диспетчер syscall'ов. Работает уже в Ring 0 (мы сюда попали через
 * interrupt gate 0x80), поэтому может напрямую вызывать k-функции
 * ядра. Аргументы — сырые uint32_t из регистров пользователя:
 * указатели валидны как есть, потому что модель памяти плоская
 * (сегменты Ring 3 — та же база 0 и лимит 4 ГиБ, отличается только
 * DPL). Вернёт значение в EAX пользователя asm-обработчик. */
int32_t syscall_dispatch(uint32_t nr, uint32_t a1, uint32_t a2,
                         uint32_t a3, uint32_t a4, uint32_t a5)
{
    (void)a4; (void)a5;
    switch (nr) {
    case SYS_KOPEN:   return kopen((const char *)a1, (int)a2);
    case SYS_KCLOSE:  return kclose((int)a1);
    case SYS_KREAD:   return kread((int)a1, (void *)a2, a3);
    case SYS_KWRITE:  return kwrite((int)a1, (const void *)a2, a3);
    case SYS_KSEEK:   return kseek((int)a1, a2, (int)a3);
    case SYS_KUNLINK: return kunlink((const char *)a1);
    case SYS_KPRINT:  kprint((const char *)a1); return 0;
    case SYS_KEXECVE: return kexecve((const char *)a1, (char *const *)a2,
                                     (char *const *)a3); /* успех — без возврата */
    case SYS_KALLOC:  return (int32_t)kpmm_alloc();
    case SYS_KFREE:   kpmm_free((void *)a1); return 0;
    case SYS_KCLEAR:  video_clear(); return 0;
    default:          return -22;   /* -EINVAL: нет такого syscall'а */
    }
}
