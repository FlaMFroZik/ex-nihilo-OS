#include <stdint.h>
#include <stddef.h>
#include "hal/pmm.h"

/* Текстовый видеобуфер VGA: 80x25 ячеек, каждая ячейка — 2 байта:
 *   байт символа и байт атрибута (цвет текста и фона). */
#define VGA_MEM   ((volatile uint8_t *)0xB8000)
#define VGA_COLS  80
#define VGA_ROWS  25
#define VGA_ATTR  0x07          /* светло-серый текст на черном фоне */

/* Записывает один символ в ячейку (row, col).
 *   Возвращает колонку, в которую нужно писать следующий символ. */
static unsigned int put_char(unsigned int row, unsigned int col, char c)
{
    if (row >= VGA_ROWS || col >= VGA_COLS)
        return col;                     /* за пределами экрана: пропускаем */

    volatile uint8_t *cell = VGA_MEM + (row * VGA_COLS + col) * 2;
    cell[0] = (uint8_t)c;               /* символ */
    cell[1] = VGA_ATTR;                 /* атрибут */
    return col + 1;
}

/* Прокрутка экрана на одну строку вверх: строки 1..24 -> 0..23.
 *   Нужна потому, что карта E820 бывает длиннее экрана. */
static void scroll(void)
{
    for (unsigned int row = 1; row < VGA_ROWS; row++)
        for (unsigned int col = 0; col < VGA_COLS; col++) {
            volatile uint8_t *dst = VGA_MEM + ((row - 1) * VGA_COLS + col) * 2;
            volatile uint8_t *src = VGA_MEM + (row * VGA_COLS + col) * 2;
            dst[0] = src[0];
            dst[1] = src[1];
        }

    for (unsigned int col = 0; col < VGA_COLS; col++)
        put_char(VGA_ROWS - 1, col, ' ');
}

/* Очищает весь экран */
static void clear_screen(void)
{
    for (unsigned int row = 0; row < VGA_ROWS; row++)
        for (unsigned int col = 0; col < VGA_COLS; col++)
            put_char(row, col, ' ');
}

/* Выводит строку str, начиная с позиции (row, col).
 *   Возвращает колонку, следующую за последним символом. */
static unsigned int out(unsigned int row, unsigned int col, const char *str)
{
    while (*str != '\0')
        col = put_char(row, col, *str++);
    return col;
}

/* Выводит беззнаковое число в виде 0x + 8 шестнадцатеричных цифр.
 *   Возвращает колонку, следующую за последней цифрой. */
static unsigned int out_hex(unsigned int row, unsigned int col, uintptr_t value)
{
    static const char digits[] = "0123456789ABCDEF";

    col = out(row, col, "0x");
    for (int shift = 28; shift >= 0; shift -= 4)
        col = put_char(row, col, digits[(value >> shift) & 0xF]);
    return col;
}

/* Выводит беззнаковое число в десятичной записи, начиная с (row, col).
 *   Возвращает колонку, следующую за последней цифрой.
 *   Локальный массив здесь не используем: с защитой стека (по умолчанию в Ubuntu)
 *   gcc вставит вызов __stack_chk_fail, которого в ядре нет. */
static unsigned int out_dec(unsigned int row, unsigned int col, uintptr_t value)
{
    uintptr_t divisor = 1;

    /* Находим старший разряд: делитель, при котором value / divisor даёт первую цифру */
    while (value / divisor >= 10)
        divisor *= 10;

    /* Выводим цифры слева направо */
    for (; divisor > 0; divisor /= 10)
        col = put_char(row, col, (char)('0' + (value / divisor) % 10));

    return col;
}

/* Текущая строка вывода: все сообщения идут вниз, экран прокручивается */
static unsigned int cursor_row;

/* Начать новую строку, прокрутив экран при необходимости */
static void newline(void)
{
    cursor_row++;
    if (cursor_row >= VGA_ROWS) {
        scroll();
        cursor_row = VGA_ROWS - 1;
    }
}

/* Вывести строку целиком (с нуля), затем перейти на следующую */
static void emit(const char *str)
{
    out(cursor_row, 0, str);
    newline();
}

/* "prefix<dec>suffix" одной строкой */
static void emit_dec(const char *prefix, uintptr_t value, const char *suffix)
{
    unsigned int col = out(cursor_row, 0, prefix);
    col = out_dec(cursor_row, col, value);
    out(cursor_row, col, suffix);
    newline();
}

/* "prefix<hex>suffix" одной строкой */
static void emit_hex(const char *prefix, uintptr_t value, const char *suffix)
{
    unsigned int col = out(cursor_row, 0, prefix);
    col = out_hex(cursor_row, col, value);
    out(cursor_row, col, suffix);
    newline();
}

/* Одна запись карты E820 с полным разбором:
 *   [i] base=0x........ len=0x........ type=N free|used  pages=N */
static void emit_entry(uint32_t index, const e820_entry_t *e)
{
    unsigned int col;

    col = out(cursor_row, 0, "[");
    col = out_dec(cursor_row, col, index);
    col = out(cursor_row, col, "] base=");
    col = out_hex(cursor_row, col, (uintptr_t)e->base);
    col = out(cursor_row, col, " len=");
    col = out_hex(cursor_row, col, (uintptr_t)e->length);
    col = out(cursor_row, col, " type=");
    col = out_dec(cursor_row, col, e->type);

    /* сколько страниц по 4 КиБ приносит эта запись */
    col = out(cursor_row, col, (e->type == 1) ? " free pages=" : " used pages=");
    out_dec(cursor_row, col, (uintptr_t)(e->length / PAGE_SIZE));

    newline();
}

void kmain(void)
{
    pmm_init();
    clear_screen();
    cursor_row = 0;

    /* ================= 1. Карта памяти от BIOS ================= */
    emit("=== ex-nihilo-OS: physical memory report ===");
    emit("Memory map source: BIOS INT 15h, AX=E820h");
    emit_dec("E820 entries: ", pmm_e820_entries(),
             " (regions reported by firmware)");
    emit_dec("  of them usable (type=1): ", pmm_e820_usable(), "");

    uint32_t entries = pmm_e820_entries();
    for (uint32_t i = 0; i < entries; i++) {
        e820_entry_t e;
        if (pmm_e820_entry(i, &e))
            emit_entry(i, &e);
    }

    emit("");

    /* ================= 2. Арифметика по страницам ================= */
    emit("--- 4 KiB page accounting ---");
    emit_dec("Usable pages from E820 (type=1): ", pmm_usable_pages(),
             " x 4096 B");
    emit_dec("  = total usable RAM, KiB:        ", pmm_usable_bytes() / 1024, "");
    emit_dec("Pages reserved for kernel image:  ", pmm_kernel_pages(),
             " x 4096 B");
    emit_dec("Pages free for allocation:        ", pmm_free_pages(),
             " x 4096 B");
    emit_dec("  = free RAM, KiB:                ", pmm_free_pages() * 4, "");
    emit_dec("Highest page index in model:      ", pmm_total_pages() - 1, "");
    emit_hex("Top address covered by bitmap:    ", pmm_total_pages() * PAGE_SIZE, "");
    emit("");

    /* ================= 3. Демонстрация alloc/free ================= */
    emit("--- pmm_alloc() demo: 10 pages ---");

    void *pages[10];
    for (unsigned int i = 0; i < 10; i++) {
        pages[i] = pmm_alloc();

        unsigned int col = out(cursor_row, 0, "Page ");
        col = out_dec(cursor_row, col, i);
        col = out(cursor_row, col, ": ");

        if (pages[i] == NULL) {
            out(cursor_row, col, "OUT OF MEMORY!");
        } else {
            col = out_hex(cursor_row, col, (uintptr_t)pages[i]);
            col = out(cursor_row, col, " (index ");
            col = out_dec(cursor_row, col, (uintptr_t)pages[i] / PAGE_SIZE);
            out(cursor_row, col, ")");
        }
        newline();
    }
    emit_dec("Free pages after 10 allocations:   ", pmm_free_pages(), "");
    emit("");

    /* ================= 4. Проверка pmm_free() ================= */
    emit("--- pmm_free() behaviour ---");

    uint32_t before = pmm_free_pages();
    pmm_free(pages[0]);
    emit_dec("After free(page0) free pages:      ", pmm_free_pages(),
             (pmm_free_pages() == before + 1) ? "  [ok +1]" : "  [BUG]");

    emit_dec("Free pages now:                   ", pmm_free_pages(), "");

    /* повторное освобождение и мусорные аргументы */
    uint32_t guard = pmm_free_pages();
    pmm_free(pages[0]);                 /* двойное освобождение */
    pmm_free(NULL);                     /* NULL */
    pmm_free((void *)0x1234);           /* невыровненный адрес */
    emit_dec("After double free/NULL/unaligned: ", pmm_free_pages(),
             (pmm_free_pages() == guard) ? "  [ok, unchanged]" : "  [BUG]");

    /* попытка освободить зарезервированное */
    guard = pmm_free_pages();
    pmm_free((void *)0xB8000);          /* видеопамять VGA */
    pmm_free((void *)0x10000);          /* код ядра */
    pmm_free((void *)0xF0000);          /* BIOS ROM */
    emit_dec("After freeing VGA/kernel/ROM:     ", pmm_free_pages(),
             (pmm_free_pages() == guard) ? "  [ok, protected]" : "  [BUG]");
    emit("");

    /* ================= 5. Итог ================= */
    emit("--- final state ---");
    emit_dec("Free pages remaining:             ", pmm_free_pages(), "");
    emit_dec("Free RAM remaining, KiB:          ", pmm_free_pages() * 4, "");

    /* Всё сделано. Останавливаем процессор. */
    emit("System halted (hlt loop).");
    for (;;)
        __asm__ volatile ("hlt");
}
