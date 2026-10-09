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
    clear_screen();
    out(0, 0, "Physical pages allocated by pmm_alloc():");

    for (unsigned int i = 0; i < 10; i++) {
        unsigned int row = i + 2;       /* строки 2..11 */
        void *page = pmm_alloc();

        unsigned int col = out(row, 0, "Page ");
        col = out_dec(row, col, i);
        col = out(row, col, ": ");

        if (page == NULL)
            out(row, col, "OUT OF MEMORY!");
        else
            out_dec(row, col, (uintptr_t)page);
    }

    /* Всё сделано. Останавливаем процессор, иначе выполнение уйдет неизвестно куда */
    for (;;)
        __asm__ volatile ("hlt");
}
