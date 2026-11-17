#ifndef KAPI_H
#define KAPI_H

#include <stdint.h>
#include <stddef.h>
#include "fs/fat16.h"

/* ================================================================
 * kapi — функции ЯДРА (Ring 0) с префиксом k.
 *
 * Раньше kmain и тесты дёргали драйверы напрямую: fat16_open(),
 * pmm_alloc(), fat16_read()... Это низкоуровневые драйверные
 * имена, и знать их должен только Ring 0. Поэтому поверх них
 * сделан ядерный API, где у каждой ранее сделанной функции
 * в начале стоит префикс k:
 *
 *     fat16_open()   -> kopen()        pmm_alloc()      -> kpmm_alloc()
 *     fat16_close()  -> kclose()       pmm_free()       -> kpmm_free()
 *     fat16_read()   -> kread()        pmm_free_pages() -> kpmm_free_pages()
 *     fat16_write()  -> kwrite()
 *     fat16_seek()   -> kseek()
 *     fat16_unlink() -> kunlink()
 *     fat16_readdir()-> kreaddir()
 *     fat16_exists() -> kexists()
 *
 * Драйверные имена (fat16_*, pmm_*) НЕ переименованы: их используют
 * host-тесты (tests/) и сам код драйверов. k-функции — это именно
 * та поверхность, которую видит остальное ядро и которую вызывает
 * обработчик syscall'ов (kernel/syscall.c). Код Ring 3 эти функции
 * вызывать НЕ может: он ходит через int 0x80 (kernel/syscall.h).
 * ================================================================ */

/* Смонтировать FAT16 (ATA + fat16_mount) в глобальный том ядра.
 * Вызывается один раз из kmain (Ring 0). 0 — успех. */
int kmount(void);
int kis_mounted(void);

/* --- Файлы: fd — индекс в таблице открытых файлов ядра --- */
int kopen(const char *path, int flags);                 /* fd >= 0 или ошибка < 0 */
int kclose(int fd);
int kread(int fd, void *buf, uint32_t count);           /* байт прочитано или < 0 */
int kwrite(int fd, const void *buf, uint32_t count);    /* байт записано или < 0 */
int kseek(int fd, uint32_t offset, int whence);
int kunlink(const char *path);
int kreaddir(const char *path, uint32_t index, fat16_dirent_t *out);
int kexists(const char *path);
uint32_t kfree_clusters(void);
uint32_t ktotal_clusters(void);

/* --- Физическая память --- */
void *kpmm_alloc(void);
void kpmm_free(void *addr);
uint32_t kpmm_free_pages(void);

/* --- Исполнение программ --- */

/* Загрузить ELF32-бинарник с диска и заменить им текущий образ
 * Ring 3 (реализация и формат — kernel/exec.h). При успехе НЕ
 * возвращается; при ошибке возвращает отрицательный код, и
 * вызывающий продолжает работу. */
int kexecve(const char *path, char *const argv[], char *const envp[]);

/* --- Вывод из Ring 0 (ставится из kmain, где живёт раскладка экрана) --- */
typedef void (*kprint_fn_t)(const char *str);
void kprint_register(kprint_fn_t fn);
void kprint(const char *str);   /* no-op, пока обработчик не зарегистрирован */

#endif
