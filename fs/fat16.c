/* fs/fat16.c — драйвер FAT16 для ex-nihilo-OS.
 *
 * Драйвер работает поверх абстракции blockdev (fs/blockdev.h), поэтому
 * один и тот же код используется и в ядре (ATA PIO, hal/ata.c), и в
 * host-тестах (образ-файл через Linux-сисколлы, tests/fat_test.c).
 *
 * Динамической памяти нет: всё состояние — в структурах, которые
 * передаёт вызывающий код (fat16_volume_t, fat16_file_t).
 *
 * Поддерживается: open/read/write/seek/close/truncate, создание и
 * удаление файлов, каталоги (mkdir/rmdir/readdir), rename. Имена —
 * только 8.3 в ASCII; длинные имена (LFN) пропускаются при разборе,
 * но не создаются.
 *
 * Функции, объявленные static, идут в том порядке, в котором их можно
 * читать: секторы → строки и имена → FAT → каталоги → пути → API.
 */

#include "fs/fat16.h"

/* ==================================================================
 * 1. Секторы и числа
 * ================================================================== */

static int rd(fat16_volume_t *vol, uint32_t lba, void *buf)
{
    return vol->dev->read(vol->dev->ctx, lba, 1, buf);
}

static int wr(fat16_volume_t *vol, uint32_t lba, const void *buf)
{
    if (vol->read_only)
        return FAT16_ERR_ROFS;
    return vol->dev->write(vol->dev->ctx, lba, 1, buf);
}

static uint16_t get16(const uint8_t *p)
{
    return (uint16_t)(p[0] | (p[1] << 8));
}

static uint32_t get32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void put16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

static void put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

/* ==================================================================
 * 2. Строки и имена 8.3
 * ================================================================== */

static int strequal(const char *a, const char *b)
{
    while (*a && *b) {
        if (*a != *b)
            return 0;
        a++;
        b++;
    }
    return *a == *b;
}

/* Допустимые символы имени в FAT (подмножество, без национальных). */
static int is_name_char(char c)
{
    static const char allowed[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-~!#$%&'()@^`{}";

    for (const char *a = allowed; *a; a++)
        if (*a == c)
            return 1;
    return 0;
}

static char upper(char c)
{
    return (c >= 'a' && c <= 'z') ? (char)(c - 'a' + 'A') : c;
}

/* "name.ext" -> 11 байт каталога FAT ("NAME    EXT").
 * Возвращает 0 или FAT16_ERR_INVAL. */
static int name_to_83(const char *name, uint8_t out[11])
{
    unsigned int i;

    for (i = 0; i < 11; i++)
        out[i] = ' ';

    if (*name == '\0')
        return FAT16_ERR_INVAL;

    i = 0;
    while (*name && *name != '.') {
        char c = upper(*name++);

        if (!is_name_char(c) || i >= 8)
            return FAT16_ERR_INVAL;
        out[i++] = (uint8_t)c;
    }

    if (*name == '.') {
        name++;
        i = 8;
        while (*name) {
            char c = upper(*name++);

            if (!is_name_char(c) || i >= 11)
                return FAT16_ERR_INVAL;
            out[i++] = (uint8_t)c;
        }
    }

    return FAT16_OK;
}

/* 11 байт каталога -> "NAME.EXT". */
static void name_from_83(const uint8_t *raw, char out[FAT16_MAX_NAME])
{
    unsigned int n = 0;

    for (unsigned int i = 0; i < 8; i++)
        if (raw[i] != ' ')
            out[n++] = (char)raw[i];

    if (raw[8] != ' ') {
        out[n++] = '.';
        for (unsigned int i = 8; i < 11; i++)
            if (raw[i] != ' ')
                out[n++] = (char)raw[i];
    }
    out[n] = '\0';
}

/* ==================================================================
 * 3. FAT: чтение и запись цепочек кластеров
 * ================================================================== */

static uint32_t fat_entry_lba(fat16_volume_t *vol, uint32_t cluster)
{
    return vol->fat_start_lba + (cluster * 2) / FAT16_SECTOR_SIZE;
}

static uint32_t fat_entry_off(uint32_t cluster)
{
    return (cluster * 2) % FAT16_SECTOR_SIZE;
}

static int cluster_valid(fat16_volume_t *vol, uint32_t cluster)
{
    return cluster >= 2 && cluster < vol->cluster_count + 2;
}

/* Значение FAT для кластера; FAT16_EOC при ошибке чтения. */
static uint32_t fat_get(fat16_volume_t *vol, uint32_t cluster)
{
    uint8_t sec[FAT16_SECTOR_SIZE];

    if (!cluster_valid(vol, cluster))
        return FAT16_EOC;
    if (rd(vol, fat_entry_lba(vol, cluster), sec) != 0)
        return FAT16_EOC;
    return get16(sec + fat_entry_off(cluster));
}

/* Записать значение во ВСЕ копии FAT — иначе копии разойдутся. */
static int fat_set(fat16_volume_t *vol, uint32_t cluster, uint32_t value)
{
    uint8_t sec[FAT16_SECTOR_SIZE];

    if (!cluster_valid(vol, cluster))
        return FAT16_ERR_INVAL;

    for (uint8_t copy = 0; copy < vol->num_fats; copy++) {
        uint32_t lba = fat_entry_lba(vol, cluster) + (uint32_t)copy * vol->fat_size;

        if (rd(vol, lba, sec) != 0)
            return FAT16_ERR_IO;
        put16(sec + fat_entry_off(cluster), (uint16_t)value);
        if (wr(vol, lba, sec) != 0)
            return FAT16_ERR_IO;
    }
    return FAT16_OK;
}

static uint32_t cluster_lba(fat16_volume_t *vol, uint32_t cluster)
{
    return vol->data_start_lba + (cluster - 2) * vol->sectors_per_cluster;
}

/* Выделить кластер, сразу помеченный как конец цепочки. 0 — нет места. */
static uint32_t cluster_alloc(fat16_volume_t *vol)
{
    for (uint32_t c = 2; c < vol->cluster_count + 2; c++) {
        if (fat_get(vol, c) == 0) {
            if (fat_set(vol, c, FAT16_EOC) != 0)
                return 0;
            return c;
        }
    }
    return 0;
}

/* Освободить всю цепочку. Ограничение счётчиком защищает от
 * зацикленной или испорченной цепочки. */
static int cluster_free_chain(fat16_volume_t *vol, uint32_t first)
{
    for (uint32_t guard = 0; cluster_valid(vol, first); guard++) {
        uint32_t next = fat_get(vol, first);

        if (guard > vol->cluster_count)
            return FAT16_ERR_IO;
        if (fat_set(vol, first, 0) != FAT16_OK)
            return FAT16_ERR_IO;
        if (next == FAT16_EOC || next == 0 || next == FAT16_BAD || next > FAT16_BAD)
            return FAT16_OK;
        first = next;
    }
    return FAT16_OK;
}

/* Кластер номер index в цепочке (0 — первый). 0 — цепочка короче. */
static uint32_t chain_at(fat16_volume_t *vol, uint32_t first, uint32_t index)
{
    uint32_t guard = 0;

    while (index > 0 && cluster_valid(vol, first)) {
        first = fat_get(vol, first);
        if (first == FAT16_EOC || first == 0 || first >= FAT16_BAD)
            return 0;
        index--;
        if (++guard > vol->cluster_count)
            return 0;
    }
    return cluster_valid(vol, first) ? first : 0;
}

/* Убедиться, что цепочка содержит минимум want кластеров, при
 * необходимости дописав новые. В last_out — последний кластер. */
static int chain_ensure(fat16_volume_t *vol, uint32_t first, uint32_t want,
                        uint32_t *last_out)
{
    uint32_t cur = first;
    uint32_t have = 1;

    if (want == 0)
        return FAT16_OK;
    if (!cluster_valid(vol, first))
        return FAT16_ERR_INVAL;

    while (have < want) {
        uint32_t next = fat_get(vol, cur);

        if (next == 0 || next == FAT16_EOC || next >= FAT16_BAD) {
            uint32_t fresh = cluster_alloc(vol);

            if (fresh == 0)
                return FAT16_ERR_NOSPC;
            if (fat_set(vol, cur, fresh) != FAT16_OK)
                return FAT16_ERR_IO;
            cur = fresh;
        } else {
            cur = next;
        }
        if (++have > vol->cluster_count)
            return FAT16_ERR_IO;
    }

    if (last_out)
        *last_out = cur;
    return FAT16_OK;
}

/* ==================================================================
 * 4. Каталоги: записи по индексу
 * ================================================================== */

/* Область каталога: либо фиксированный корень (first_cluster == 0),
 * либо цепочка кластеров подкаталога. */
typedef struct {
    uint32_t first_cluster;
    uint32_t size;              /* байт */
} dir_ref_t;

/* Сектор, в котором лежит запись номер index. */
static int dir_sector(fat16_volume_t *vol, const dir_ref_t *dir,
                      uint32_t index, uint32_t *lba_out)
{
    uint32_t byte_off = index * 32;

    if (dir->first_cluster == 0) {
        uint32_t sector = byte_off / FAT16_SECTOR_SIZE;

        if (sector >= vol->root_dir_sectors)
            return FAT16_ERR_NOENT;
        *lba_out = vol->root_start_lba + sector;
        return FAT16_OK;
    } else {
        uint32_t cl_index = byte_off / vol->cluster_bytes;
        uint32_t in_cluster = byte_off % vol->cluster_bytes;
        uint32_t cluster = chain_at(vol, dir->first_cluster, cl_index);

        if (cluster == 0)
            return FAT16_ERR_NOENT;
        *lba_out = cluster_lba(vol, cluster) + in_cluster / FAT16_SECTOR_SIZE;
        return FAT16_OK;
    }
}

static int dir_read_entry(fat16_volume_t *vol, const dir_ref_t *dir,
                          uint32_t index, uint8_t entry[32])
{
    uint8_t sec[FAT16_SECTOR_SIZE];
    uint32_t lba;

    if (dir_sector(vol, dir, index, &lba) != FAT16_OK)
        return FAT16_ERR_NOENT;
    if (rd(vol, lba, sec) != 0)
        return FAT16_ERR_IO;

    uint32_t off = (index * 32) % FAT16_SECTOR_SIZE;

    for (unsigned int i = 0; i < 32; i++)
        entry[i] = sec[off + i];
    return FAT16_OK;
}

static int dir_write_entry(fat16_volume_t *vol, const dir_ref_t *dir,
                           uint32_t index, const uint8_t entry[32])
{
    uint8_t sec[FAT16_SECTOR_SIZE];
    uint32_t lba;

    if (dir_sector(vol, dir, index, &lba) != FAT16_OK)
        return FAT16_ERR_INVAL;
    if (rd(vol, lba, sec) != 0)
        return FAT16_ERR_IO;

    uint32_t off = (index * 32) % FAT16_SECTOR_SIZE;

    for (unsigned int i = 0; i < 32; i++)
        sec[off + i] = entry[i];
    return wr(vol, lba, sec);
}

/* Живая запись: не свободная, не LFN, не метка тома. */
static int dir_entry_live(const uint8_t *e)
{
    if (e[0] == 0x00 || e[0] == 0xE5)
        return 0;
    if (e[11] == 0x0F)                  /* LFN */
        return 0;
    if (e[11] & FAT16_ATTR_VOLUME_ID)
        return 0;
    return 1;
}

/* "." или ".." — при переборе их не показываем. */
static int is_dot_entry(const uint8_t *e)
{
    if (e[0] != '.')
        return 0;
    return e[1] == ' ' || (e[1] == '.' && e[2] == ' ');
}

static int dir_find(fat16_volume_t *vol, const dir_ref_t *dir,
                    const uint8_t name83[11], uint32_t *index_out,
                    uint32_t *lba_out)
{
    uint32_t entries = dir->size / 32;

    for (uint32_t i = 0; i < entries; i++) {
        /* ВАЖНО: rd() читает целый сектор (512 Б), поэтому буфер
         * обязан быть не меньше сектора — иначе затирается стек. */
        uint8_t sec[FAT16_SECTOR_SIZE];
        uint32_t lba;

        if (dir_sector(vol, dir, i, &lba) != FAT16_OK)
            return FAT16_ERR_NOENT;
        if (rd(vol, lba, sec) != 0)
            return FAT16_ERR_IO;

        const uint8_t *entry = sec + (i * 32) % FAT16_SECTOR_SIZE;

        if (entry[0] == 0x00)           /* дальше записей нет */
            return FAT16_ERR_NOENT;
        if (!dir_entry_live(entry))
            continue;

        int same = 1;

        for (unsigned int k = 0; k < 11; k++)
            if (entry[k] != name83[k])
                same = 0;
        if (same) {
            if (index_out)
                *index_out = i;
            if (lba_out)
                *lba_out = lba;
            return FAT16_OK;
        }
    }
    return FAT16_ERR_NOENT;
}

/* Найти свободную запись (0x00 или 0xE5). */
static int dir_alloc_entry(fat16_volume_t *vol, const dir_ref_t *dir,
                           uint32_t *index_out, uint32_t *lba_out)
{
    uint32_t entries = dir->size / 32;

    for (uint32_t i = 0; i < entries; i++) {
        uint8_t sec[FAT16_SECTOR_SIZE];
        uint32_t lba;

        if (dir_sector(vol, dir, i, &lba) != FAT16_OK)
            break;
        if (rd(vol, lba, sec) != 0)
            return FAT16_ERR_IO;

        uint8_t first = sec[(i * 32) % FAT16_SECTOR_SIZE];

        if (first == 0x00 || first == 0xE5) {
            *index_out = i;
            *lba_out = lba;
            return FAT16_OK;
        }
    }
    return FAT16_ERR_NOSPC;
}

/* ==================================================================
 * 5. Обход пути
 *
 * Поддерживается произвольная вложенность, разделитель — '/'.
 * "." и ".." не разворачиваются: драйвер ждёт канонические пути.
 * ================================================================== */

#define FAT16_MAX_DEPTH 8

static void dir_root(fat16_volume_t *vol, dir_ref_t *dir)
{
    dir->first_cluster = 0;
    dir->size = (uint32_t)vol->root_entries * 32;
}

/* Спуститься в подкаталог по записи name83 в parent. */
static int dir_enter(fat16_volume_t *vol, const dir_ref_t *parent,
                     const uint8_t name83[11], dir_ref_t *child)
{
    uint32_t index;
    uint8_t entry[32];
    int rc = dir_find(vol, parent, name83, &index, 0);

    if (rc != FAT16_OK)
        return rc;
    if (dir_read_entry(vol, parent, index, entry) != FAT16_OK)
        return FAT16_ERR_IO;
    if (!(entry[11] & FAT16_ATTR_DIRECTORY))
        return FAT16_ERR_NOTDIR;

    child->first_cluster = get16(entry + 26);
    child->size = get32(entry + 28);
    if (child->size == 0)
        child->size = vol->cluster_bytes;   /* пустой каталог */
    return FAT16_OK;
}

/* Разобрать путь.
 *   parent — каталог, содержащий последний компонент;
 *   name83 — последний компонент в формате 8.3;
 *   self   — описание каталога, на который указывает путь, если это
 *            каталог (для "/" — корень); может быть 0.
 */
static int path_resolve(fat16_volume_t *vol, const char *path,
                        dir_ref_t *parent, uint8_t name83[11],
                        dir_ref_t *self)
{
    dir_ref_t cur;
    uint32_t depth = 0;

    while (*path == '/')
        path++;
    if (*path == '\0')
        return FAT16_ERR_INVAL;

    dir_root(vol, &cur);
    if (self)
        *self = cur;

    for (;;) {
        const char *end = path;
        char comp[FAT16_MAX_NAME];
        uint8_t comp83[11];
        unsigned int len = 0;

        while (*end && *end != '/') {
            if (len + 1 >= sizeof(comp))
                return FAT16_ERR_INVAL;
            comp[len++] = *end++;
        }
        comp[len] = '\0';

        if (name_to_83(comp, comp83) != FAT16_OK)
            return FAT16_ERR_INVAL;

        while (*end == '/')
            end++;

        if (*end == '\0') {
            for (unsigned int i = 0; i < 11; i++)
                name83[i] = comp83[i];
            *parent = cur;
            return FAT16_OK;
        }

        if (++depth > FAT16_MAX_DEPTH)
            return FAT16_ERR_INVAL;

        int rc = dir_enter(vol, &cur, comp83, &cur);

        if (rc != FAT16_OK)
            return rc;
        if (self)
            *self = cur;
        path = end;
    }
}

/* ==================================================================
 * 6. Монтирование
 * ================================================================== */

int fat16_mount(blockdev_t *dev, uint32_t base_lba, fat16_volume_t *vol)
{
    uint8_t bs[FAT16_SECTOR_SIZE];
    uint8_t *raw = (uint8_t *)vol;

    for (unsigned int i = 0; i < sizeof(*vol); i++)
        raw[i] = 0;

    if (dev == 0)
        return FAT16_ERR_INVAL;

    vol->dev = dev;
    vol->base_lba = base_lba;

    if (dev->read(dev->ctx, base_lba, 1, bs) != 0)
        return FAT16_ERR_IO;
    if (bs[510] != 0x55 || bs[511] != 0xAA)
        return FAT16_ERR_INVAL;

    vol->bytes_per_sector = get16(bs + 11);
    vol->sectors_per_cluster = bs[13];
    vol->reserved_sectors = get16(bs + 14);
    vol->num_fats = bs[16];
    vol->root_entries = get16(bs + 17);
    vol->total_sectors16 = get16(bs + 19);
    vol->fat_size = get16(bs + 22);
    vol->total_sectors32 = get32(bs + 32);

    if (vol->bytes_per_sector != FAT16_SECTOR_SIZE || vol->sectors_per_cluster == 0)
        return FAT16_ERR_INVAL;
    if (vol->num_fats == 0 || vol->fat_size == 0 || vol->root_entries == 0)
        return FAT16_ERR_INVAL;

    uint32_t total = vol->total_sectors16 ? vol->total_sectors16
                                          : vol->total_sectors32;

    if (total == 0 || total < vol->reserved_sectors)
        return FAT16_ERR_INVAL;

    vol->root_dir_sectors = (uint32_t)(vol->root_entries * 32 + 511) / 512;
    vol->fat_start_lba = base_lba + vol->reserved_sectors;
    vol->root_start_lba = vol->fat_start_lba + (uint32_t)vol->num_fats * vol->fat_size;
    vol->data_start_lba = vol->root_start_lba + vol->root_dir_sectors;

    uint32_t overhead = vol->data_start_lba - base_lba;

    if (overhead >= total)
        return FAT16_ERR_INVAL;

    vol->cluster_count = (total - overhead) / vol->sectors_per_cluster;
    vol->cluster_bytes = (uint32_t)vol->sectors_per_cluster * FAT16_SECTOR_SIZE;

    /* FAT16 обязана иметь >= 4085 кластеров, иначе это FAT12 */
    if (vol->cluster_count < 4085)
        return FAT16_ERR_INVAL;

    vol->read_only = (dev->write == 0) ? 1 : 0;
    return FAT16_OK;
}

/* ==================================================================
 * 7. Файлы: открытие, закрытие, чтение, запись
 * ================================================================== */

static void file_from_entry(fat16_file_t *file, const uint8_t entry[32],
                            uint32_t lba, uint32_t index)
{
    file->used = 1;
    file->first_cluster = get16(entry + 26);
    file->size = get32(entry + 28);
    file->pos = 0;
    file->dir_ent_lba = lba;
    file->dir_ent_off = (index * 32) % FAT16_SECTOR_SIZE;
    file->dirty = 0;
    name_from_83(entry, file->name);
}

/* Новая запись каталога для создаваемого файла. */
static void file_new_entry(fat16_volume_t *vol, fat16_file_t *file,
                           const uint8_t name83[11], uint32_t lba, uint32_t index)
{
    for (unsigned int i = 0; i < sizeof(*file); i++)
        ((uint8_t *)file)[i] = 0;

    file->used = 1;
    file->first_cluster = 0;
    file->size = 0;
    file->pos = 0;
    file->dir_ent_lba = lba;
    file->dir_ent_off = (index * 32) % FAT16_SECTOR_SIZE;
    file->dirty = 1;                    /* запись уже создана на диске */
    name_from_83(name83, file->name);
    (void)vol;
}

int fat16_open(fat16_volume_t *vol, const char *path, int flags,
               fat16_file_t *file)
{
    dir_ref_t parent;
    uint8_t name83[11];
    uint32_t index, lba;
    uint8_t entry[32];
    int rc;

    if (file == 0)
        return FAT16_ERR_INVAL;

    for (unsigned int i = 0; i < sizeof(*file); i++)
        ((uint8_t *)file)[i] = 0;

    rc = path_resolve(vol, path, &parent, name83, 0);
    if (rc != FAT16_OK)
        return rc;

    rc = dir_find(vol, &parent, name83, &index, &lba);

    if (rc == FAT16_ERR_NOENT) {
        uint32_t new_index, new_lba;
        uint8_t fresh[32];

        if (!(flags & FAT16_O_CREAT))
            return FAT16_ERR_NOENT;
        if (vol->read_only)
            return FAT16_ERR_ROFS;
        if (dir_alloc_entry(vol, &parent, &new_index, &new_lba) != FAT16_OK)
            return FAT16_ERR_NOSPC;

        for (unsigned int i = 0; i < 32; i++)
            fresh[i] = 0;
        for (unsigned int i = 0; i < 11; i++)
            fresh[i] = name83[i];
        fresh[11] = FAT16_ATTR_ARCHIVE;

        if (dir_write_entry(vol, &parent, new_index, fresh) != FAT16_OK)
            return FAT16_ERR_IO;

        file_new_entry(vol, file, name83, new_lba, new_index);
        file->flags = flags;
        return FAT16_OK;
    }

    if (rc != FAT16_OK)
        return rc;
    if (dir_read_entry(vol, &parent, index, entry) != FAT16_OK)
        return FAT16_ERR_IO;
    if (entry[11] & FAT16_ATTR_DIRECTORY)
        return FAT16_ERR_ISDIR;

    int wants_write = (flags & (FAT16_O_WRONLY | FAT16_O_RDWR)) != 0;

    if (wants_write && (entry[11] & FAT16_ATTR_READONLY))
        return FAT16_ERR_ROFS;
    if (wants_write && vol->read_only)
        return FAT16_ERR_ROFS;

    file_from_entry(file, entry, lba, index);
    file->flags = flags;

    if (flags & FAT16_O_TRUNC) {
        if (!wants_write)
            return FAT16_ERR_INVAL;
        if (file->first_cluster >= 2) {
            rc = cluster_free_chain(vol, file->first_cluster);
            if (rc != FAT16_OK)
                return rc;
        }
        file->first_cluster = 0;
        file->size = 0;
        file->dirty = 1;
    }

    if (flags & FAT16_O_APPEND)
        file->pos = file->size;

    return FAT16_OK;
}

/* Записать first_cluster/size обратно в DIR-запись. */
static int file_flush_entry(fat16_volume_t *vol, fat16_file_t *file)
{
    uint8_t sec[FAT16_SECTOR_SIZE];

    if (!file->dirty)
        return FAT16_OK;
    if (rd(vol, file->dir_ent_lba, sec) != 0)
        return FAT16_ERR_IO;

    put16(sec + file->dir_ent_off + 20, 0);        /* старшее слово кластера */
    put16(sec + file->dir_ent_off + 26, (uint16_t)file->first_cluster);
    put32(sec + file->dir_ent_off + 28, file->size);

    if (wr(vol, file->dir_ent_lba, sec) != 0)
        return FAT16_ERR_IO;

    file->dirty = 0;
    return FAT16_OK;
}

int fat16_close(fat16_volume_t *vol, fat16_file_t *file)
{
    int rc;

    if (file == 0 || !file->used)
        return FAT16_ERR_INVAL;

    rc = file_flush_entry(vol, file);
    file->used = 0;
    return rc;
}

/* Сектор, в котором лежит байт file_off файла. */
static int file_io_sector(fat16_volume_t *vol, fat16_file_t *file,
                          uint32_t file_off, uint8_t *sec, int is_write)
{
    uint32_t cluster = chain_at(vol, file->first_cluster,
                                file_off / vol->cluster_bytes);

    if (cluster == 0)
        return FAT16_ERR_IO;

    uint32_t in_cluster = file_off % vol->cluster_bytes;
    uint32_t lba = cluster_lba(vol, cluster) + in_cluster / FAT16_SECTOR_SIZE;

    return is_write ? wr(vol, lba, sec) : rd(vol, lba, sec);
}

/* Выделить кластеры так, чтобы существовал байт file_off. */
static int file_ensure_space(fat16_volume_t *vol, fat16_file_t *file,
                             uint32_t file_off)
{
    uint32_t need = file_off / vol->cluster_bytes + 1;

    if (file->first_cluster < 2) {
        uint32_t fresh = cluster_alloc(vol);

        if (fresh == 0)
            return FAT16_ERR_NOSPC;
        file->first_cluster = fresh;
        file->dirty = 1;
        if (need == 1)
            return FAT16_OK;
    }

    return chain_ensure(vol, file->first_cluster, need, 0);
}

int fat16_read(fat16_volume_t *vol, fat16_file_t *file, void *buf, uint32_t count)
{
    uint8_t *out = (uint8_t *)buf;
    uint32_t done = 0;

    if (file == 0 || !file->used || buf == 0)
        return FAT16_ERR_INVAL;
    if (count == 0 || file->pos >= file->size)
        return 0;
    if (count > file->size - file->pos)
        count = file->size - file->pos;

    while (done < count) {
        uint8_t sec[FAT16_SECTOR_SIZE];
        uint32_t file_off = file->pos + done;
        uint32_t in_sec = file_off % FAT16_SECTOR_SIZE;
        uint32_t chunk = FAT16_SECTOR_SIZE - in_sec;

        if (chunk > count - done)
            chunk = count - done;
        if (file_io_sector(vol, file, file_off, sec, 0) != 0)
            return done ? (int)done : FAT16_ERR_IO;

        for (uint32_t i = 0; i < chunk; i++)
            out[done + i] = sec[in_sec + i];
        done += chunk;
    }

    file->pos += done;
    return (int)done;
}

/* Записать кусок, который может не покрывать сектор целиком:
 * недостающее читаем с диска, чтобы не затереть соседние данные. */
static int write_partial(fat16_volume_t *vol, fat16_file_t *file,
                         uint32_t file_off, const uint8_t *data, uint32_t len)
{
    uint8_t sec[FAT16_SECTOR_SIZE];
    uint32_t in_sec = file_off % FAT16_SECTOR_SIZE;

    if (in_sec == 0 && len == FAT16_SECTOR_SIZE) {
        for (unsigned int i = 0; i < FAT16_SECTOR_SIZE; i++)
            sec[i] = data[i];
    } else {
        if (file_io_sector(vol, file, file_off, sec, 0) != 0)
            for (unsigned int i = 0; i < FAT16_SECTOR_SIZE; i++)
                sec[i] = 0;
        for (uint32_t i = 0; i < len; i++)
            sec[in_sec + i] = data[i];
    }

    return file_io_sector(vol, file, file_off, sec, 1);
}

int fat16_write(fat16_volume_t *vol, fat16_file_t *file, const void *buf,
                uint32_t count)
{
    const uint8_t *in = (const uint8_t *)buf;
    uint32_t done = 0;

    if (file == 0 || !file->used || buf == 0)
        return FAT16_ERR_INVAL;
    if (vol->read_only)
        return FAT16_ERR_ROFS;
    if (count == 0)
        return 0;

    while (done < count) {
        uint32_t file_off = file->pos + done;
        uint32_t in_sec = file_off % FAT16_SECTOR_SIZE;
        uint32_t chunk = FAT16_SECTOR_SIZE - in_sec;

        if (chunk > count - done)
            chunk = count - done;
        if (file_ensure_space(vol, file, file_off) != FAT16_OK)
            return done ? (int)done : FAT16_ERR_NOSPC;
        if (write_partial(vol, file, file_off, in + done, chunk) != 0)
            return done ? (int)done : FAT16_ERR_IO;

        done += chunk;
    }

    file->pos += done;
    if (file->pos > file->size) {
        file->size = file->pos;
        file->dirty = 1;
    }
    return (int)done;
}

int fat16_seek(fat16_volume_t *vol, fat16_file_t *file, uint32_t offset, int whence)
{
    uint32_t base;

    (void)vol;

    if (file == 0 || !file->used)
        return FAT16_ERR_INVAL;

    switch (whence) {
    case 0: base = 0; break;
    case 1: base = file->pos; break;
    case 2: base = file->size; break;
    default: return FAT16_ERR_INVAL;
    }

    if (base + offset < base)               /* переполнение 32 бит */
        return FAT16_ERR_INVAL;

    file->pos = base + offset;
    return (int)file->pos;
}

int fat16_truncate(fat16_volume_t *vol, fat16_file_t *file, uint32_t size)
{
    int rc;

    if (file == 0 || !file->used)
        return FAT16_ERR_INVAL;
    if (vol->read_only)
        return FAT16_ERR_ROFS;

    if (size == 0) {
        if (file->first_cluster >= 2) {
            rc = cluster_free_chain(vol, file->first_cluster);
            if (rc != FAT16_OK)
                return rc;
        }
        file->first_cluster = 0;
        file->size = 0;
        file->pos = 0;
        file->dirty = 1;
        return FAT16_OK;
    }

    if (size > file->size) {                /* удлиняем */
        rc = file_ensure_space(vol, file, size - 1);
        if (rc != FAT16_OK)
            return rc;
        file->size = size;
        file->dirty = 1;
        return FAT16_OK;
    }

    /* укорачиваем: лишние кластеры — в 0, последний — EOC */
    uint32_t keep = (size + vol->cluster_bytes - 1) / vol->cluster_bytes;
    uint32_t last = 0;

    rc = chain_ensure(vol, file->first_cluster, keep, &last);
    if (rc != FAT16_OK)
        return rc;

    if (cluster_valid(vol, last)) {
        uint32_t rest = fat_get(vol, last);

        if (cluster_valid(vol, rest)) {
            rc = cluster_free_chain(vol, rest);
            if (rc != FAT16_OK)
                return rc;
        }
        if (fat_set(vol, last, FAT16_EOC) != FAT16_OK)
            return FAT16_ERR_IO;
    }

    file->size = size;
    if (file->pos > size)
        file->pos = size;
    file->dirty = 1;
    return FAT16_OK;
}

/* ==================================================================
 * 8. Каталоги: создание, удаление, перебор
 * ================================================================== */

static void make_dot(uint8_t out[11], int dots)
{
    for (unsigned int i = 0; i < 11; i++)
        out[i] = ' ';
    out[0] = '.';
    if (dots == 2)
        out[1] = '.';
}

int fat16_mkdir(fat16_volume_t *vol, const char *path)
{
    dir_ref_t parent;
    uint8_t name83[11], dot[11], dotdot[11];
    uint32_t index, lba;
    uint8_t entry[32];
    uint8_t sec[FAT16_SECTOR_SIZE];
    int rc;

    if (vol->read_only)
        return FAT16_ERR_ROFS;

    rc = path_resolve(vol, path, &parent, name83, 0);
    if (rc != FAT16_OK)
        return rc;

    rc = dir_find(vol, &parent, name83, &index, &lba);
    if (rc == FAT16_OK)
        return FAT16_ERR_EXIST;
    if (rc != FAT16_ERR_NOENT)
        return rc;

    uint32_t new_index, new_lba;

    if (dir_alloc_entry(vol, &parent, &new_index, &new_lba) != FAT16_OK)
        return FAT16_ERR_NOSPC;

    uint32_t cluster = cluster_alloc(vol);

    if (cluster == 0)
        return FAT16_ERR_NOSPC;

    for (unsigned int i = 0; i < 32; i++)
        entry[i] = 0;
    for (unsigned int i = 0; i < 11; i++)
        entry[i] = name83[i];
    entry[11] = FAT16_ATTR_DIRECTORY;
    put16(entry + 26, (uint16_t)cluster);

    if (dir_write_entry(vol, &parent, new_index, entry) != FAT16_OK)
        return FAT16_ERR_IO;

    /* внутри нового каталога — "." и ".." */
    for (unsigned int i = 0; i < FAT16_SECTOR_SIZE; i++)
        sec[i] = 0;

    make_dot(dot, 1);
    make_dot(dotdot, 2);

    for (unsigned int i = 0; i < 11; i++)
        sec[i] = dot[i];
    sec[11] = FAT16_ATTR_DIRECTORY;
    put16(sec + 26, (uint16_t)cluster);

    for (unsigned int i = 0; i < 11; i++)
        sec[32 + i] = dotdot[i];
    sec[32 + 11] = FAT16_ATTR_DIRECTORY;
    /* ".." в каталоге первого уровня указывает на корень — кластер 0 */
    put16(sec + 32 + 26, (uint16_t)parent.first_cluster);

    if (wr(vol, cluster_lba(vol, cluster), sec) != 0)
        return FAT16_ERR_IO;

    return FAT16_OK;
}

int fat16_rmdir(fat16_volume_t *vol, const char *path)
{
    dir_ref_t parent, target;
    uint8_t name83[11];
    uint32_t index, lba;
    uint8_t entry[32];
    int rc;

    if (vol->read_only)
        return FAT16_ERR_ROFS;

    rc = path_resolve(vol, path, &parent, name83, 0);
    if (rc != FAT16_OK)
        return rc;

    rc = dir_find(vol, &parent, name83, &index, &lba);
    if (rc != FAT16_OK)
        return rc;
    if (dir_read_entry(vol, &parent, index, entry) != FAT16_OK)
        return FAT16_ERR_IO;
    if (!(entry[11] & FAT16_ATTR_DIRECTORY))
        return FAT16_ERR_NOTDIR;

    target.first_cluster = get16(entry + 26);
    target.size = vol->cluster_bytes;

    /* каталог должен быть пуст, кроме "." и ".." */
    for (uint32_t i = 2; i < target.size / 32; i++) {
        uint8_t e[32];

        if (dir_read_entry(vol, &target, i, e) != FAT16_OK)
            break;
        if (e[0] == 0x00)
            break;
        if (e[0] == 0xE5)
            continue;
        return FAT16_ERR_NOTEMPTY;
    }

    if (cluster_valid(vol, target.first_cluster)) {
        rc = cluster_free_chain(vol, target.first_cluster);
        if (rc != FAT16_OK)
            return rc;
    }

    uint8_t sec[FAT16_SECTOR_SIZE];

    if (rd(vol, lba, sec) != 0)
        return FAT16_ERR_IO;
    sec[(index * 32) % FAT16_SECTOR_SIZE] = 0xE5;
    return wr(vol, lba, sec);
}

int fat16_unlink(fat16_volume_t *vol, const char *path)
{
    dir_ref_t parent;
    uint8_t name83[11];
    uint32_t index, lba;
    uint8_t entry[32];
    int rc;

    if (vol->read_only)
        return FAT16_ERR_ROFS;

    rc = path_resolve(vol, path, &parent, name83, 0);
    if (rc != FAT16_OK)
        return rc;

    rc = dir_find(vol, &parent, name83, &index, &lba);
    if (rc != FAT16_OK)
        return rc;
    if (dir_read_entry(vol, &parent, index, entry) != FAT16_OK)
        return FAT16_ERR_IO;
    if (entry[11] & FAT16_ATTR_DIRECTORY)
        return FAT16_ERR_ISDIR;
    if (entry[11] & FAT16_ATTR_READONLY)
        return FAT16_ERR_ROFS;

    uint32_t first = get16(entry + 26);

    if (cluster_valid(vol, first)) {
        rc = cluster_free_chain(vol, first);
        if (rc != FAT16_OK)
            return rc;
    }

    uint8_t sec[FAT16_SECTOR_SIZE];

    if (rd(vol, lba, sec) != 0)
        return FAT16_ERR_IO;
    sec[(index * 32) % FAT16_SECTOR_SIZE] = 0xE5;
    return wr(vol, lba, sec);
}

int fat16_rename(fat16_volume_t *vol, const char *from, const char *to)
{
    dir_ref_t parent;
    uint8_t old83[11], new83[11];
    uint32_t index, lba, dummy_i, dummy_l;
    uint8_t sec[FAT16_SECTOR_SIZE];
    int rc;

    if (vol->read_only)
        return FAT16_ERR_ROFS;

    rc = path_resolve(vol, from, &parent, old83, 0);
    if (rc != FAT16_OK)
        return rc;
    rc = path_resolve(vol, to, &parent, new83, 0);
    if (rc != FAT16_OK)
        return rc;

    rc = dir_find(vol, &parent, old83, &index, &lba);
    if (rc != FAT16_OK)
        return rc;
    if (dir_find(vol, &parent, new83, &dummy_i, &dummy_l) == FAT16_OK)
        return FAT16_ERR_EXIST;

    if (rd(vol, lba, sec) != 0)
        return FAT16_ERR_IO;

    uint8_t *e = sec + (index * 32) % FAT16_SECTOR_SIZE;

    for (unsigned int i = 0; i < 11; i++)
        e[i] = new83[i];
    return wr(vol, lba, sec);
}

int fat16_readdir(fat16_volume_t *vol, const char *path, uint32_t index,
                  fat16_dirent_t *out)
{
    dir_ref_t dir, self;
    uint8_t name83[11];
    uint32_t seen = 0;

    if (out == 0)
        return FAT16_ERR_INVAL;

    if (path == 0 || path[0] == '\0' || strequal(path, "/")) {
        dir_root(vol, &dir);
    } else {
        uint32_t index_in_root;
        uint8_t entry[32];

        int rc = path_resolve(vol, path, &dir, name83, &self);

        if (rc != FAT16_OK)
            return rc;

        /* путь должен указывать на каталог: проверяем его DIR-запись */
        if (self.first_cluster != 0) {
            dir = self;                 /* уже подкаталог, найденный по пути */
        } else {
            dir_ref_t root;

            dir_root(vol, &root);
            if (dir_find(vol, &root, name83, &index_in_root, 0) != FAT16_OK)
                return FAT16_ERR_NOENT;
            if (dir_read_entry(vol, &root, index_in_root, entry) != FAT16_OK)
                return FAT16_ERR_IO;
            if (!(entry[11] & FAT16_ATTR_DIRECTORY))
                return FAT16_ERR_NOTDIR;
            dir.first_cluster = get16(entry + 26);
            dir.size = get32(entry + 28);
            if (dir.size == 0)
                dir.size = vol->cluster_bytes;
        }
    }

    for (uint32_t i = 0; i < dir.size / 32; i++) {
        uint8_t e[32];

        if (dir_read_entry(vol, &dir, i, e) != FAT16_OK)
            return 0;
        if (e[0] == 0x00)
            return 0;
        if (!dir_entry_live(e) || is_dot_entry(e))
            continue;
        if (seen == index) {
            name_from_83(e, out->name);
            out->size = get32(e + 28);
            out->first_cluster = get16(e + 26);
            out->attr = e[11];
            return 1;
        }
        seen++;
    }
    return 0;
}

int fat16_exists(fat16_volume_t *vol, const char *path)
{
    dir_ref_t parent;
    uint8_t name83[11];

    if (path_resolve(vol, path, &parent, name83, 0) != FAT16_OK)
        return 0;
    return dir_find(vol, &parent, name83, 0, 0) == FAT16_OK;
}

uint32_t fat16_free_clusters(fat16_volume_t *vol)
{
    uint32_t free_count = 0;

    for (uint32_t c = 2; c < vol->cluster_count + 2; c++)
        if (fat_get(vol, c) == 0)
            free_count++;
    return free_count;
}

uint32_t fat16_total_clusters(fat16_volume_t *vol)
{
    return vol->cluster_count;
}

int fat16_sync(fat16_volume_t *vol)
{
    /* Кэша нет: каждая fat_set() пишет обе копии FAT сразу, а
     * dir_write_entry() сразу пишет сектор каталога. Функция
     * оставлена ради симметричного API. */
    (void)vol;
    return FAT16_OK;
}
