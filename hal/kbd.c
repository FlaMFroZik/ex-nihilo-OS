#include <stdint.h>
#include "hal/kbd.h"

#define PS2_DATA    0x60
#define PS2_STATUS  0x64
#define COM1_DATA   0x3F8
#define COM1_LSR    0x3FD

static inline uint8_t inb(uint16_t port)
{
    uint8_t v;
    __asm__ volatile("inb %1, %0" : "=a"(v) : "Nd"(port));
    return v;
}

static const char sc_normal[128] = {
    0,  27, '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '-', '=', '\b',
    '\t', 'q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p', '[', ']', '\n',
    0, 'a', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l', ';', '\'', '`',
    0, '\\', 'z', 'x', 'c', 'v', 'b', 'n', 'm', ',', '.', '/', 0,
    '*', 0, ' '
};

static const char sc_shift[128] = {
    0,  27, '!', '@', '#', '$', '%', '^', '&', '*', '(', ')', '_', '+', '\b',
    '\t', 'Q', 'W', 'E', 'R', 'T', 'Y', 'U', 'I', 'O', 'P', '{', '}', '\n',
    0, 'A', 'S', 'D', 'F', 'G', 'H', 'J', 'K', 'L', ':', '"', '~',
    0, '|', 'Z', 'X', 'C', 'V', 'B', 'N', 'M', '<', '>', '?', 0,
    '*', 0, ' '
};

int kbd_getc(void)
{
    static int shift = 0;
    static int caps = 0;

    for (;;) {
        if (inb(COM1_LSR) & 0x01) {
            uint8_t c = inb(COM1_DATA);
            if (c == '\r')
                return '\n';
            if (c == 0x7F)
                return '\b';
            return (int)c;
        }

        if (inb(PS2_STATUS) & 0x01) {
            uint8_t sc = inb(PS2_DATA);
            if (sc == 0x2A || sc == 0x36) {
                shift = 1;
                continue;
            }
            if (sc == 0xAA || sc == 0xB6) {
                shift = 0;
                continue;
            }
            if (sc == 0x3A) {
                caps = !caps;
                continue;
            }
            if (sc & 0x80)
                continue;
            if (sc < 128) {
                char c = shift ? sc_shift[sc] : sc_normal[sc];
                if (caps && c >= 'a' && c <= 'z')
                    c = (char)(c - 'a' + 'A');
                else if (caps && c >= 'A' && c <= 'Z')
                    c = (char)(c - 'A' + 'a');
                if (c != 0)
                    return (int)(uint8_t)c;
            }
        }
    }
}
