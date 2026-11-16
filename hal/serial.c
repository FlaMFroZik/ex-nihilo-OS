/* hal/serial.c — COM1, только запись.
 *
 * Регистры 16550 в режиме без FIFO-настроек:
 *   0x3F8 + 0 — данные (запись) / приём (чтение)
 *   0x3F8 + 1 — разрешение прерываний
 *   0x3F8 + 2 — FIFO
 *   0x3F8 + 3 — управление линией (формат кадра)
 *   0x3F8 + 4 — управление модемом (DTR/RTS)
 *   0x3F8 + 5 — состояние линии: бит 5 = регистр данных пуст
 *
 * Порт может отсутствовать (например, при `-serial none`): тогда
 * чтение статуса даёт 0xFF, и мы просто не ждём вечно — проверка
 * ограничена попытками.
 */

#include "hal/serial.h"

#define COM1 0x3F8

static inline void outb(unsigned short port, unsigned char v)
{
    __asm__ volatile("outb %0, %1" : : "a"(v), "Nd"(port));
}

static inline unsigned char inb(unsigned short port)
{
    unsigned char v;

    __asm__ volatile("inb %1, %0" : "=a"(v) : "Nd"(port));
    return v;
}

void serial_init(void)
{
    outb(COM1 + 1, 0x00);       /* без прерываний */
    outb(COM1 + 3, 0x80);       /* DLAB=1: задаём делитель */
    outb(COM1 + 0, 0x01);       /* 115200 / 1 = 115200 бод */
    outb(COM1 + 1, 0x00);
    outb(COM1 + 3, 0x03);       /* 8 бит, без контроля чётности, 1 стоп */
    outb(COM1 + 2, 0xC7);       /* FIFO вкл., очистить, порог 14 */
    outb(COM1 + 4, 0x0B);       /* DTR + RTS + OUT2 */
}

/* Ждём готовности передатчика. 100000 попыток — с большим запасом:
 * если порта нет, статус читается как 0xFF и цикл завершится сразу. */
static void wait_tx(void)
{
    for (unsigned int i = 0; i < 100000; i++)
        if (inb(COM1 + 5) & 0x20)
            return;
}

void serial_putc(char c)
{
    if (c == '\n')
        serial_putc('\r');

    wait_tx();
    outb(COM1 + 0, (unsigned char)c);
}

void serial_write(const char *s)
{
    while (*s)
        serial_putc(*s++);
}
