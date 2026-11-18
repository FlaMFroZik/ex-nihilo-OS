#ifndef PAGING_H
#define PAGING_H

#include <stdint.h>

/* ================================================================
 * paging — защита памяти на страничной адресации i386.
 *
 * До этого память была плоской: сегменты Ring 3 имели ту же базу 0
 * и лимит 4 ГиБ, что и ядро, — Ring 3 мог читать и писать куда
 * угодно, отличался только CPL. Теперь включён пейджинг
 * (CR0.PG | CR0.WP):
 *
 *   * вся RAM отображена тождественно (vaddr == phys), но страницы
 *     ядра помечены Supervisor (U/S=0): из Ring 3 они недоступны
 *     ни на чтение, ни на запись, ни на исполнение управления —
 *     попытка даёт #PF;
 *   * пользовательскими (U/S=1) помечаются только то, что Ring 3
 *     положено: регион влинкованного user-кода ядра, его стек,
 *     сегменты загруженного ELF (с учётом PF_W) и его стек;
 *   * #PF из Ring 3 не валит систему: обработчик фиксирует адрес и
 *     завершает провинившийся код — enter_usermode() возвращает
 *     USER_EXIT_PAGEFAULT. #PF в Ring 0 — паника ядра.
 * ================================================================ */

/* Построить каталог страниц на всю RAM из E820-модели PMM,
 * отобразить LFB видеобуфера, поставить gate #PF в IDT и включить
 * пейджинг. Вызывать один раз, после pmm_init() и video_init(). */
void paging_init(void);

/* Пометить диапазон страниц [vaddr, vaddr+size): user!=0 — доступна
 * из Ring 3, writable!=0 — доступна на запись. Для адресов вне RAM
 * (LFB) создаёт отображение phys == vaddr. Работает и до, и после
 * включения пейджинга (с инвалидацией TLB). */
void paging_map(uint32_t vaddr, uint32_t size, int user, int writable);

/* Диагностика последнего #PF из Ring 3 (для отчёта в kmain). */
uint32_t paging_last_fault_addr(void);
uint32_t paging_last_fault_err(void);
int paging_fault_count(void);

/* Обработчики из C для asm-входа #PF (kernel/syscall.asm). */
void page_fault_note(uint32_t cr2, uint32_t err, uint32_t eip);
__attribute__((noreturn)) void page_fault_panic(uint32_t cr2, uint32_t err,
                                               uint32_t eip);

#endif
