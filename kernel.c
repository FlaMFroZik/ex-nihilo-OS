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

/* Вывести строку с текущей позиции и перейти на следующую.
 *   Прокручивает экран, когда строки кончились. */
static void emit(const char *str, unsigned int indent)
{
    if (cursor_row >= VGA_ROWS) {
        scroll();
        cursor_row = VGA_ROWS - 1;
    }
    out(cursor_row, indent, str);
    cursor_row++;
}

/* То же, но с числом в конце строки */
static void emit_dec(const char *prefix, uintptr_t value, const char *suffix)
{
    unsigned int col = out(cursor_row, 0, prefix);
    col = out_dec(cursor_row, col, value);
    out(cursor_row, col, suffix);
    cursor_row++;
}

/* Строка с hex-числом в середине: "base=0x........ len=0x........ type=N" */
static void emit_entry(const char *label, uintptr_t base,
                       uintptr_t length, uintptr_t type)
{
    unsigned int col = out(cursor_row, 1, label);
    col = out_hex(cursor_row, col, base);
    col = out(cursor_row, col, " len=");
    col = out_hex(cursor_row, col, length);
    col = out(cursor_row, col, " type=");
    out_dec(cursor_row, col, type);
    cursor_row++;
}

void kmain(void)
{
    pmm_init();
    clear_screen();
    cursor_row = 0;

    emit("E820 memory map from BIOS (INT 15h/AX=E820h):", 0);

    uint32_t entries = pmm_e820_entries();
    uint32_t usable  = pmm_e820_usable();

    /* "Entries: 6, usable: 3" одной строкой, без склейки col вручную */
    {
        unsigned int col = out(cursor_row, 0, "Entries: ");
        col = out_dec(cursor_row, col, entries);
        col = out(cursor_row, col, ", usable: ");
        out_dec(cursor_row, col, usable);
        cursor_row++;
    }

    /* Карта бывает длиннее экрана: показываем ВСЕ записи,
     * лишние строки прокручиваются вверх */
    for (uint32_t i = 0; i < entries; i++) {
        e820_entry_t e;

        if (!pmm_e820_entry(i, &e))
            continue;

        /* base и length — 64-битные: показываем младшие 32 бита,
         * всё, что выше, всё равно вне модели (см. hal/pmm.asm) */
        emit_entry("base=", (uintptr_t)e.base, (uintptr_t)e.length, e.type);
    }

    emit_dec("PMM usable RAM (KiB): ", pmm_usable_bytes() / 1024, "");

    {
        unsigned int col = out(cursor_row, 0, "Pages total/free: ");
        col = out_dec(cursor_row, col, pmm_total_pages());
        col = out(cursor_row, col, "/");
        out_dec(cursor_row, col, pmm_free_pages());
        cursor_row++;
    }

    /* --- выделяем 10 страниц --- */
    void *pages[10];

    for (unsigned int i = 0; i < 10; i++) {
        void *page = pmm_alloc();
        pages[i] = page;

        unsigned int col = out(cursor_row, 0, "Page ");
        col = out_dec(cursor_row, col, i);
        col = out(cursor_row, col, ": ");

        if (page == NULL)
            out(cursor_row, col, "OUT OF MEMORY!");
        else
            out_hex(cursor_row, col, (uintptr_t)page);

        cursor_row++;
    }

    /* --- проверка pmm_free() --- */
    emit_dec("Free pages before free(): ", pmm_free_pages(), "");

    pmm_free(pages[0]);

    emit_dec("Free pages after  free(): ", pmm_free_pages(), "");

    /* Повторное освобождение того же адреса не должно менять состояние */
    pmm_free(pages[0]);
    pmm_free(NULL);             /* NULL игнорируется */
    pmm_free((void *)0x1234);   /* невыровненный адрес игнорируется */

    emit_dec("Free pages after double free / bad args: ", pmm_free_pages(), "");

    /* --- попытка освободить видеопамять и код ядра: должно быть
     *     проигнорировано (иначе ОС отдала бы их под данные) --- */
    {
        uint32_t before = pmm_free_pages();
        pmm_free((void *)0xB8000);      /* видеопамять */
        pmm_free((void *)0x10000);      /* код ядра */

        unsigned int col = out(cursor_row, 0, "Reserved free attempts changed counter: ");
        out_dec(cursor_row, col, before != pmm_free_pages());
        cursor_row++;
    }

    /* Освобождённая страница должна снова выдаваться аллокатором */
    void *again = pmm_alloc();
    {
        unsigned int col = out(cursor_row, 0, "Reallocated page: ");
        out_hex(cursor_row, col, (uintptr_t)again);
        cursor_row++;
    }

    /* Освобождаем всё остальное */
    for (unsigned int i = 1; i < 10; i++)
        pmm_free(pages[i]);
    pmm_free(again);

    emit_dec("Free pages at the end: ", pmm_free_pages(), "");

    /* Всё сделано. Останавливаем процессор, иначе выполнение уйдёт неизвестно куда */
    for (;;)
        __asm__ volatile ("hlt");
}
