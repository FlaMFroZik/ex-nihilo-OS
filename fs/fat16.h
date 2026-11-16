#ifndef FAT16_H
#define FAT16_H

#include <stdint.h>
#include <stddef.h>
#include "fs/blockdev.h"

/* FAT16: файловая система, расположенная в образе ex-nihilo-OS.
 *
 * Раскладка образа (ёмкость 16 МиБ, сектор 512 Б):
 *     сектор 0       — первый этап загрузчика (boot.asm)
 *     секторы 1..2   — второй этап (boot2.asm)
 *     секторы 3..63  — ядро (kernel.bin) с запасом под рост
 *     сектор 64 ..   — FAT16 (BPB, две FAT, корневой каталог, данные)
 *
 * Почему 16 МиБ, а не 1.44 МБ: FAT16 по спецификации требует
 * count_of_clusters >= 4085, иначе ФС обязана называться FAT12.
 * На дискете 1.44 МБ максимум ~2880 кластеров, то есть честной
 * FAT16 там не бывает.
 *
 * Почему HDD, а не дискета: ядру нужен драйвер диска (в защищённом
 * режиме INT 13h недоступен). ATA PIO на HDD — это простые REP INSW/
 * OUTSW без DMA и моторов; запись на дискету в PM означала бы работу
 * с FDC, каналом DMA и таймингами — источник ошибок без выгоды.
 */

#define FAT16_SECTOR_SIZE   512
#define FAT16_MAX_NAME      12      /* "NAME.EXT" + '\0' */
#define FAT16_MAX_FDS       16      /* одновременно открытых файлов */
#define FAT16_EOC           0xFFFF  /* конец цепочки кластеров */
#define FAT16_BAD           0xFFF7  /* помеченный сбойный кластер */

/* Коды ошибок (отрицательные, как в POSIX) */
#define FAT16_OK             0
#define FAT16_ERR_NOENT    (-2)     /* нет такого файла */
#define FAT16_ERR_IO       (-5)     /* ошибка ввода-вывода */
#define FAT16_ERR_NOSPC    (-28)    /* нет места */
#define FAT16_ERR_EXIST   (-17)     /* файл уже есть */
#define FAT16_ERR_INVAL   (-22)     /* неверный аргумент */
#define FAT16_ERR_NOMEM   (-12)     /* кончились дескрипторы */
#define FAT16_ERR_NOTDIR  (-20)     /* не каталог */
#define FAT16_ERR_ISDIR   (-21)     /* это каталог */
#define FAT16_ERR_NOTEMPTY (-39)    /* каталог не пуст */
#define FAT16_ERR_ROFS    (-30)     /* том только для чтения */

/* Режимы open() */
#define FAT16_O_RDONLY  0x0
#define FAT16_O_WRONLY  0x1
#define FAT16_O_RDWR    0x2
#define FAT16_O_CREAT   0x40
#define FAT16_O_TRUNC   0x200
#define FAT16_O_APPEND  0x400

/* Тип записи каталога */
#define FAT16_ATTR_READONLY  0x01
#define FAT16_ATTR_HIDDEN    0x02
#define FAT16_ATTR_SYSTEM    0x04
#define FAT16_ATTR_VOLUME_ID 0x08
#define FAT16_ATTR_DIRECTORY 0x10
#define FAT16_ATTR_ARCHIVE   0x20

/* Данные разобранного суперблока / BPB. */
typedef struct {
    uint32_t base_lba;          /* сектор, с которого начинается том */
    uint16_t bytes_per_sector;  /* всегда 512 */
    uint8_t  sectors_per_cluster;
    uint16_t reserved_sectors;
    uint8_t  num_fats;
    uint16_t root_entries;
    uint16_t total_sectors16;
    uint32_t total_sectors32;   /* если total_sectors16 == 0 */
    uint16_t fat_size;          /* секторов на одну FAT */
    uint16_t root_dir_sectors;  /* вычисляется из root_entries */
    uint32_t first_data_sector; /* начало области данных */
    uint32_t cluster_count;     /* сколько кластеров в томе */
    uint32_t cluster_bytes;     /* sectors_per_cluster * 512 */
    uint32_t fat_start_lba;     /* абсолютный LBA первой FAT */
    uint32_t root_start_lba;    /* абсолютный LBA корневого каталога */
    uint32_t data_start_lba;    /* абсолютный LBA области данных */
    blockdev_t *dev;            /* устройство, с которого читаем */
    int      read_only;         /* 1 — записи запрещены */
} fat16_volume_t;

/* Описание одного открытого файла. Остаётся в структуре пользователя,
 * ядро только заполняет его — динамической памяти не требуется. */
typedef struct {
    int      used;              /* 1 — дескриптор занят */
    int      flags;             /* режим открытия */
    char     name[FAT16_MAX_NAME];
    uint32_t first_cluster;     /* первый кластер цепочки */
    uint32_t size;              /* размер файла в байтах */
    uint32_t pos;               /* текущая позиция чтения/записи */
    uint32_t dir_ent_lba;       /* где лежит DIR-запись файла */
    uint32_t dir_ent_off;       /* смещение записи внутри сектора */
    int      dirty;             /* 1 — нужно обновить размер в каталоге */
} fat16_file_t;

/* Данные запроса к readdir(), заполняются по одной записи. */
typedef struct {
    char     name[FAT16_MAX_NAME];
    uint32_t size;
    uint32_t first_cluster;
    uint8_t  attr;
} fat16_dirent_t;

/* ---- Инициализация ---- */

/* Прочитать BPB и подготовить том. 0 — успех. */
int fat16_mount(blockdev_t *dev, uint32_t base_lba, fat16_volume_t *vol);

/* ---- Работа с файлами ---- */

/* Открыть файл. 0 — успех, иначе отрицательный код ошибки.
 * Файл заполняет fat16_file_t, указанный вызывающим кодом. */
int fat16_open(fat16_volume_t *vol, const char *path, int flags,
               fat16_file_t *file);

/* Закрыть файл; при необходимости дописать размер в каталог. */
int fat16_close(fat16_volume_t *vol, fat16_file_t *file);

/* Прочитать не больше count байт, вернуть число прочитанных байт
 * или отрицательный код ошибки. */
int fat16_read(fat16_volume_t *vol, fat16_file_t *file, void *buf, uint32_t count);

/* Записать count байт (при необходимости выделяя кластеры и создавая
 * файл), вернуть число записанных байт или отрицательный код ошибки. */
int fat16_write(fat16_volume_t *vol, fat16_file_t *file, const void *buf,
                uint32_t count);

/* Переместить позицию. whence: 0 — начало, 1 — текущая, 2 — конец. */
int fat16_seek(fat16_volume_t *vol, fat16_file_t *file, uint32_t offset, int whence);

/* Удалить файл и освободить все его кластеры. */
int fat16_unlink(fat16_volume_t *vol, const char *path);

/* Создать каталог (однокластерный). */
int fat16_mkdir(fat16_volume_t *vol, const char *path);

/* Удалить пустой каталог. */
int fat16_rmdir(fat16_volume_t *vol, const char *path);

/* Переименовать файл внутри одного каталога. */
int fat16_rename(fat16_volume_t *vol, const char *from, const char *to);

/* Обрезать файл до длины size (умеет и удлинять). */
int fat16_truncate(fat16_volume_t *vol, fat16_file_t *file, uint32_t size);

/* ---- Каталоги ---- */

/* Сколько записей в каталоге (не считая удалённых, LFN, тома). */
int fat16_readdir(fat16_volume_t *vol, const char *path, uint32_t index,
                  fat16_dirent_t *out);

/* Есть ли такой файл/каталог: 1 — да, 0 — нет. */
int fat16_exists(fat16_volume_t *vol, const char *path);

/* Сколько кластеров свободно (по первой FAT). */
uint32_t fat16_free_clusters(fat16_volume_t *vol);

/* Сколько всего кластеров в томе. */
uint32_t fat16_total_clusters(fat16_volume_t *vol);

/* Записать FAT и DIR-записи на диск. */
int fat16_sync(fat16_volume_t *vol);

#endif
