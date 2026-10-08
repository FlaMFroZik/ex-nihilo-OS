#include <stdint.h>
#include <stddef.h>
#include "hal/pmm.h"

void out(char* str) {
    char *vidptr = (char*)0xB8000;
    unsigned int i = 0;
    unsigned int j = 0;

    // Очищаем экран
    while(j < 80 * 25 * 2) {
        vidptr[j] = ' ';
        vidptr[j+1] = 0x07;
        j += 2;
    }

    j = 0;
    // Выводим строку
    while(str[j] != '\0') {
        vidptr[i] = str[j];
        vidptr[i+1] = 0x07;
        j++;
        i += 2;
    }
}

void kmain(void)
{


    for (int i = 0; i < 10; i++) {
        void *page = pmm_alloc();
        if (!page) {
            out("OUT OF MEMORY!");
        } else {
            int adr = page;

            int *arcn;

            char *adrc;

            for (adr) {

            }
        }
    }
}
