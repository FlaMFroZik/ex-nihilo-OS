#ifndef PMM_H
#define PMM_H

#include <stdint.h>
#include <stddef.h>

#define PAGE_SHIFT      12
#define PAGE_SIZE       4096
/* 1 бит на страницу: 1 МиБ bitmap покрывает 4 ГиБ адресного пространства. */
#define PMM_MAX_PAGES   (1024u * 1024u * 8u)
#define PMM_MAX_BYTES   ((uint64_t)PMM_MAX_PAGES * PAGE_SIZE)

/* Запись карты памяти BIOS (INT 15h, AX=E820h), как её отдаёт BIOS. */
typedef struct {
    uint64_t base;
    uint64_t length;
    uint32_t type;              /* 1 = доступна ОС, 2 = занята/зарезервирована */
    uint32_t acpi_ext;          /* расширенные атрибуты ACPI */
} __attribute__((packed)) e820_entry_t;

/* Построить модель памяти по карте E820, собранной загрузчиком.
 * Если карты нет, используется нижняя граница памяти от BIOS (int 12h). */
void pmm_init(void);

/* Выделить физическую страницу. NULL — если памяти не осталось. */
void *pmm_alloc(void);

/* Освободить страницу, полученную из pmm_alloc().
 * NULL, невыровненные адреса, адреса вне модели памяти, недоступные
 * по E820 страницы и зарезервированные области (ядро, стек) игнорируются. */
void pmm_free(void *addr);

/* Сколько страниц сейчас свободно. */
uint32_t pmm_free_pages(void);

/* Сколько страниц покрывает модель памяти (page_limit). */
uint32_t pmm_total_pages(void);

/* Объём доступной ОЗУ в байтах (page_limit * PAGE_SIZE). */
uint32_t pmm_usable_bytes(void);

/* Диагностика карты E820, полученной от BIOS. */
uint32_t pmm_e820_entries(void);                        /* сколько записей */
uint32_t pmm_e820_usable(void);                         /* сколько из них type == 1 */
int      pmm_e820_entry(uint32_t index, e820_entry_t *out);  /* 1 — успех, 0 — нет такой */

#endif
