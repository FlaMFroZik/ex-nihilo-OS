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

void kmain(void)
{
    pmm_init();
    clear_screen();

    unsigned int row = 0;
    unsigned int col;

    col = out(row++, 0, "Physical pages allocated by pmm_alloc():");
    (void)col;

    void *pages[10];

    for (unsigned int i = 0; i < 10; i++) {
        void *page = pmm_alloc();
        pages[i] = page;

        col = out(row, 0, "Page ");
        col = out_dec(row, col, i);
        col = out(row, col, ": ");

        if (page == NULL)
            out(row, col, "OUT OF MEMORY!");
        else
            out_hex(row, col, (uintptr_t)page);

        row++;
    }

    /* --- проверка pmm_free() --- */
    col = out(row++, 0, "Free pages before free(): ");
    out_dec(row - 1, col, pmm_free_pages());

    /* Освобождаем первую страницу и проверяем, что счётчик вырос */
    pmm_free(pages[0]);

    col = out(row++, 0, "Free pages after  free(): ");
    out_dec(row - 1, col, pmm_free_pages());

    /* Повторное освобождение того же адреса не должно менять состояние */
    pmm_free(pages[0]);
    pmm_free(NULL);             /* NULL игнорируется */
    pmm_free((void *)0x1234);   /* невыровненный адрес игнорируется */

    col = out(row++, 0, "Free pages after double free / bad args: ");
    out_dec(row - 1, col, pmm_free_pages());

    /* Освобождённая страница должна снова выдаваться аллокатором */
    void *again = pmm_alloc();
    col = out(row++, 0, "Reallocated page: ");
    out_hex(row - 1, col, (uintptr_t)again);

    /* Освобождаем всё остальное */
    for (unsigned int i = 1; i < 10; i++)
        pmm_free(pages[i]);
    pmm_free(again);

    col = out(row++, 0, "Free pages at the end: ");
    out_dec(row - 1, col, pmm_free_pages());

    /* Всё сделано. Останавливаем процессор, иначе выполнение уйдёт неизвестно куда */
    for (;;)
        __asm__ volatile ("hlt");
}
