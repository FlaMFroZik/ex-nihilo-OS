#ifndef BLOCKDEV_H
#define BLOCKDEV_H

#include <stdint.h>

/* Абстракция блочного устройства: всё, что нужно файловой системе.
 *
 * Нужна, чтобы FAT16 не знал, откуда берутся секторы. В ядре это
 * ATA PIO (hal/ata.c), в host-тестах — обычный файл через Linux-
 * сисколлы (tests/fat_test.c). Благодаря этому драйвер ФС можно
 * гонять на реальных образах, не поднимая эмулятор.
 *
 * Оба метода возвращают 0 при успехе и -1 при ошибке.
 */
typedef struct blockdev {
    int (*read)(void *ctx, uint32_t lba, uint32_t count, void *buf);
    int (*write)(void *ctx, uint32_t lba, uint32_t count, const void *buf);
    void *ctx;                  /* состояние драйвера (не трогаем) */
    uint32_t sectors;           /* сколько секторов всего на устройстве */
} blockdev_t;

#endif
