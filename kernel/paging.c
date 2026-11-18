#include <stdint.h>
#include <stddef.h>
#include "kernel/paging.h"
#include "hal/pmm.h"
#include "hal/video.h"
#include "hal/serial.h"

#define PTE_PRESENT  0x001u
#define PTE_RW       0x002u
#define PTE_USER     0x004u

#define PAGE_MASK    0xFFFFF000u

/* Каталог обязан быть выровнен на 4096 (это адрес для CR3), но
 * SUBALIGN(4) у .bss в linker.ld выравнивание переменной ломает —
 * поэтому каталог выделяем с запасом и ровняем адрес руками. */
static uint32_t pd_storage[1024 + 1024];
static uint32_t *page_directory;
static int paging_on;

static uint32_t fault_addr, fault_err;
static int fault_count;

uint32_t paging_last_fault_addr(void) { return fault_addr; }
uint32_t paging_last_fault_err(void)  { return fault_err; }
int paging_fault_count(void)          { return fault_count; }

extern void idt_set_gate(uint8_t vec, uint32_t handler, uint8_t type_attr);
extern void page_fault_entry(void);

static void invlpg(uint32_t vaddr)
{
    if (paging_on)
        __asm__ volatile ("invlpg (%0)" :: "r"(vaddr) : "memory");
}

static uint32_t *table_for(uint32_t vaddr, int create)
{
    uint32_t pdi = vaddr >> 22;

    if ((page_directory[pdi] & PTE_PRESENT) == 0) {
        if (!create)
            return NULL;
        uint32_t *tbl = pmm_alloc();
        if (tbl == NULL)
            return NULL;
        for (int i = 0; i < 1024; i++)
            tbl[i] = 0;
        /* PDE сразу ставим максимально разрешающей: финальную
         * защиту определяет PTE, а не PDE. */
        page_directory[pdi] = (uint32_t)(uintptr_t)tbl |
                              PTE_PRESENT | PTE_RW | PTE_USER;
    }
    return (uint32_t *)(uintptr_t)(page_directory[pdi] & PAGE_MASK);
}

void paging_map(uint32_t vaddr, uint32_t size, int user, int writable)
{
    if (size == 0)
        return;
    uint32_t first = vaddr & PAGE_MASK;
    uint32_t last = (vaddr + size - 1) & PAGE_MASK;

    for (uint32_t va = first;; va += PAGE_SIZE) {
        uint32_t *tbl = table_for(va, 1);
        if (tbl != NULL) {
            tbl[(va >> 12) & 0x3FF] = va | PTE_PRESENT |
                (writable ? PTE_RW : 0) | (user ? PTE_USER : 0);
            invlpg(va);
        }
        if (va == last)
            break;
    }
}

void paging_init(void)
{
    page_directory = (uint32_t *)(uintptr_t)
        (((uintptr_t)pd_storage + PAGE_SIZE - 1) & ~(uintptr_t)(PAGE_SIZE - 1));

    /* Каталог — в .bss, а полагаться на её обнуление нельзя:
     * чистим сами, прежде чем строить таблицы. */
    for (int i = 0; i < 1024; i++)
        page_directory[i] = 0;

    uint64_t mem_top64 = (uint64_t)pmm_total_pages() * PAGE_SIZE;
    uint32_t mem_top = (mem_top64 > 0xFFFFFFFFull)
                       ? 0xFFFFFFFFu : (uint32_t)mem_top64;

    /* Вся RAM — тождественно, Supervisor, RW. Пользовательские
     * куски поверх этого перепометят paging_map(..., user=1). */
    for (uint32_t va = 0; va < mem_top; va += PAGE_SIZE) {
        uint32_t *tbl = table_for(va, 1);
        if (tbl == NULL)
            break;
        tbl[(va >> 12) & 0x3FF] = va | PTE_PRESENT | PTE_RW;
        if (va == mem_top - PAGE_SIZE)
            break;
    }

    /* Видеобуфер живёт вне RAM (0xE0000000 у QEMU): отобразить. */
    uint32_t lfb = video_framebuffer();
    if (lfb != 0)
        paging_map(lfb, video_framebuffer_size(), 0, 1);

    idt_set_gate(14, (uint32_t)(uintptr_t)page_fault_entry, 0x8E);

    /* CR3 = каталог (физический == виртуальный, отображение
     * тождественное), CR0 |= PG|WP: WP заставляет и Ring 0 уважать
     * read-only страницы. */
    __asm__ volatile (
        "mov %0, %%cr3\n"
        "mov %%cr0, %%eax\n"
        "or $0x80010000, %%eax\n"
        "mov %%eax, %%cr0\n"
        :: "r"((uint32_t)(uintptr_t)page_directory) : "eax", "memory");
    paging_on = 1;
}

/* --- #PF: вызывается из asm-входа (kernel/syscall.asm) --- */

void page_fault_note(uint32_t cr2, uint32_t err, uint32_t eip)
{
    (void)eip;
    fault_addr = cr2;
    fault_err = err;
    fault_count++;
}

static void serial_hex(uint32_t v)
{
    static const char digits[] = "0123456789ABCDEF";
    char buf[11];
    buf[0] = '0'; buf[1] = 'x';
    for (int i = 0; i < 8; i++)
        buf[2 + i] = digits[(v >> (28 - 4 * i)) & 0xF];
    buf[10] = '\0';
    serial_write(buf);
}

__attribute__((noreturn)) void page_fault_panic(uint32_t cr2, uint32_t err,
                                               uint32_t eip)
{
    serial_write("\n[BUG] kernel page fault at ");
    serial_hex(cr2);
    serial_write(" err=");
    serial_hex(err);
    serial_write(" eip=");
    serial_hex(eip);
    serial_write(" - halted\n");
    for (;;)
        __asm__ volatile ("cli; hlt");
}
