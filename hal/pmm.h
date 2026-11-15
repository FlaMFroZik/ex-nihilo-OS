#ifndef PMM_H
#define PMM_H

#include <stdint.h>
#include <stddef.h>

#define PAGE_SIZE       4096
#define PMM_PAGE_COUNT  1024                        /* 4 МиБ */
#define PMM_TOTAL_BYTES (PMM_PAGE_COUNT * PAGE_SIZE)

/* Инициализация: все страницы свободны, кроме
 *   0x00000..kernel_end  — загрузчик, код и данные ядра, BDA;
 *   0x80000..0xFFFFF     — стек ядра, EBDA, видеопамять VGA, BIOS ROM. */
void  pmm_init(void);

/* Выделить физическую страницу. NULL — если памяти не осталось. */
void *pmm_alloc(void);

/* Освободить страницу, полученную из pmm_alloc().
 * NULL, невыровненные адреса, адреса вне модели памяти и
 * зарезервированные области игнорируются. */
void  pmm_free(void *addr);

/* Сколько страниц сейчас свободно. */
uint32_t pmm_free_pages(void);

#endif
