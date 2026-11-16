/* hal/ata.c — привязка драйвера ATA PIO к абстракции blockdev.
 *
 * Сам драйвер (порты, ожидание BSY/DRQ, REP INSW/OUTSW) написан на
 * ассемблере в hal/ata.asm. Здесь только тонкая обёртка, приводящая
 * его сигнатуры к fs/blockdev.h, чтобы FAT16 мог работать с диском,
 * не зная про порты ввода-вывода.
 */

#include "hal/ata.h"

extern int      ata_init(void);
extern uint32_t ata_sectors(void);

/* Счётчик секторов, прочитанных и записанных: полезно и для отчёта
 * ядра, и для диагностики «ФС молча ничего не делает». */
static uint32_t ata_reads;
static uint32_t ata_writes;

static int ata_read(void *ctx, uint32_t lba, uint32_t count, void *buf)
{
    uint8_t *p = (uint8_t *)buf;

    (void)ctx;

    for (uint32_t i = 0; i < count; i++) {
        if (ata_read_sector(lba + i, p) != 0)
            return -1;
        p += ATA_SECTOR_SIZE;
        ata_reads++;
    }
    return 0;
}

static int ata_write(void *ctx, uint32_t lba, uint32_t count, const void *buf)
{
    const uint8_t *p = (const uint8_t *)buf;

    (void)ctx;

    for (uint32_t i = 0; i < count; i++) {
        if (ata_write_sector(lba + i, p) != 0)
            return -1;
        p += ATA_SECTOR_SIZE;
        ata_writes++;
    }
    return 0;
}

int ata_blockdev(blockdev_t *dev)
{
    if (dev == 0)
        return -1;
    if (!ata_init())
        return -1;

    dev->read = ata_read;
    dev->write = ata_write;
    dev->ctx = 0;
    dev->sectors = ata_sectors();
    return 0;
}

uint32_t ata_read_count(void)
{
    return ata_reads;
}

uint32_t ata_write_count(void)
{
    return ata_writes;
}
