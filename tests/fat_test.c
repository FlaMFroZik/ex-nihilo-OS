/* tests/fat_test.c — проверка драйвера FAT16 на реальном образе.
 *
 * Тест — обычная 32-битная Linux-программа (как tests/pmm_test.c),
 * которая линкуется с настоящим fs/fat16.c. Образ передаётся
 * аргументом командной строки; доступ к нему идёт через blockdev,
 * реализованный на Linux-сисколлах (open/lseek/read/write).
 *
 * Образ готовит tools/mkfatimg.py: настоящую FAT16 создаёт независимая
 * реализация (mtools), туда же кладутся исходные файлы. Драйвер обязан
 * прочитать их байт в байт, а после его записи образ повторно
 * проверяется уже через mtools (mdir/mcopy) — обе стороны смотрят на один
 * и тот же диск глазами разных реализаций.
 */

#include <stdint.h>
#include <stddef.h>

#include "fs/fat16.h"

/* ---- Linux-сисколлы: без libc ---- */

#define SYS_exit   1
#define SYS_read   3
#define SYS_write  4
#define SYS_open   5
#define SYS_close  6
#define SYS_lseek 19

#define O_RDWR 2

static long sys_write(int fd, const void *buf, unsigned long len)
{
    long ret;
    __asm__ volatile ("int $0x80"
                      : "=a"(ret)
                      : "a"(SYS_write), "b"(fd), "c"(buf), "d"(len)
                      : "memory");
    return ret;
}

static long sys_read(int fd, void *buf, unsigned long len)
{
    long ret;
    __asm__ volatile ("int $0x80"
                      : "=a"(ret)
                      : "a"(SYS_read), "b"(fd), "c"(buf), "d"(len)
                      : "memory");
    return ret;
}

static long sys_open(const char *path, int flags)
{
    long ret;
    __asm__ volatile ("int $0x80"
                      : "=a"(ret)
                      : "a"(SYS_open), "b"(path), "c"(flags)
                      : "memory");
    return ret;
}

static long sys_close(int fd)
{
    long ret;
    __asm__ volatile ("int $0x80" : "=a"(ret) : "a"(SYS_close), "b"(fd) : "memory");
    return ret;
}

static long sys_lseek(int fd, long off, int whence)
{
    long ret;
    __asm__ volatile ("int $0x80"
                      : "=a"(ret)
                      : "a"(SYS_lseek), "b"(fd), "c"(off), "d"(whence)
                      : "memory");
    return ret;
}

/* ---- вывод ---- */

static void out(const char *s)
{
    unsigned long n = 0;

    while (s[n])
        n++;
    sys_write(1, s, n);
}

static void out_dec(long v)
{
    char buf[24];
    int i = 0;
    unsigned long u;

    if (v < 0) {
        sys_write(1, "-", 1);
        u = (unsigned long)(-v);
    } else {
        u = (unsigned long)v;
    }

    if (u == 0) {
        sys_write(1, "0", 1);
        return;
    }
    while (u) {
        buf[i++] = (char)('0' + u % 10);
        u /= 10;
    }
    while (i--)
        sys_write(1, &buf[i], 1);
}

/* ---- счётчики проверок ---- */

static unsigned long checks;
static unsigned long failures;

static void check(int cond, const char *what)
{
    checks++;
    if (!cond) {
        failures++;
        out("  [FAIL] ");
        out(what);
        out("\n");
    }
}

/* ---- blockdev поверх файла ---- */

typedef struct {
    int fd;
} filedev_t;

static int fd_read(void *ctx, uint32_t lba, uint32_t count, void *buf)
{
    filedev_t *d = (filedev_t *)ctx;
    unsigned char *p = (unsigned char *)buf;

    if (sys_lseek(d->fd, (long)lba * 512, 0) < 0)
        return -1;
    while (count--) {
        if (sys_read(d->fd, p, 512) != 512)
            return -1;
        p += 512;
    }
    return 0;
}

static int fd_write(void *ctx, uint32_t lba, uint32_t count, const void *buf)
{
    filedev_t *d = (filedev_t *)ctx;
    const unsigned char *p = (const unsigned char *)buf;

    if (sys_lseek(d->fd, (long)lba * 512, 0) < 0)
        return -1;
    while (count--) {
        if (sys_write(d->fd, p, 512) != 512)
            return -1;
        p += 512;
    }
    return 0;
}

/* ---- сами проверки ---- */

#define FAT_BASE_LBA 128

static blockdev_t dev;
static fat16_volume_t vol;

static void test_mount(void)
{
    out("Монтирование:\n");
    check(fat16_mount(&dev, FAT_BASE_LBA, &vol) == FAT16_OK,
          "fat16_mount() читает BPB");
    check(vol.bytes_per_sector == 512, "размер сектора 512");
    check(vol.cluster_count >= 4085, "кластеров >= 4085 (это FAT16, а не FAT12)");
    check(vol.num_fats == 2, "две копии FAT");
    out("  том: кластеров "); out_dec((long)vol.cluster_count);
    out(", свободно "); out_dec((long)fat16_free_clusters(&vol));
    out(", кластер "); out_dec((long)vol.cluster_bytes); out(" Б\n");
}

/* Прочитать весь файл через драйвер в буфер. Возвращает длину или -1. */
static int read_file(const char *path, unsigned char *buf, uint32_t cap)
{
    fat16_file_t f;
    int rc = fat16_open(&vol, path, FAT16_O_RDONLY, &f);

    if (rc != FAT16_OK)
        return rc;

    int total = 0;

    for (;;) {
        int n = fat16_read(&vol, &f, buf + total, cap - (uint32_t)total);

        if (n < 0) {
            fat16_close(&vol, &f);
            return -1;
        }
        if (n == 0)
            break;
        total += n;
    }
    fat16_close(&vol, &f);
    return total;
}

static void test_read_seeded(void)
{
    out("\nЧтение файлов, созданных на хосте (mtools):\n");

    /* HELLO.TXT — 12 байт */
    static unsigned char buf[8192];
    int n = read_file("/HELLO.TXT", buf, sizeof(buf));

    check(n == 12, "HELLO.TXT: длина 12");
    check(n == 12 && buf[0] == 'h' && buf[11] == '\n', "HELLO.TXT: содержимое");

    /* BIG.BIN — 3 КиБ, пересекает границу кластера */
    n = read_file("/BIG.BIN", buf, sizeof(buf));
    check(n == 3072, "BIG.BIN: длина 3072");
    check(n > 0 && buf[0] == 0 && buf[255] == 255 && buf[256] == 0,
          "BIG.BIN: данные на границе кластера");

    /* 70000 байт при кластере 2 КиБ — 35 кластеров. В буфер 8 КиБ
     * файл не влезает, поэтому проверяем только начало. */
    n = read_file("/MEDIA.BIN", buf, sizeof(buf));
    check(n == 8192, "MEDIA.BIN: read_file ограничен буфером (8192)");

    /* полностью — потоково, маленькими кусками */
    fat16_file_t f;
    check(fat16_open(&vol, "/MEDIA.BIN", FAT16_O_RDONLY, &f) == FAT16_OK,
          "MEDIA.BIN открывается");
    check(f.size == 70000, "MEDIA.BIN: размер из каталога 70000");

    unsigned char small[100];
    uint32_t total = 0;
    int bad = 0;

    for (;;) {
        int got = fat16_read(&vol, &f, small, sizeof(small));

        if (got <= 0)
            break;
        for (int i = 0; i < got; i++) {
            /* файл заполнен по формуле (offset * 7 + 13) & 0xFF */
            unsigned char want = (unsigned char)(((total + (uint32_t)i) * 7 + 13) & 0xFF);

            if (small[i] != want)
                bad = 1;
        }
        total += (uint32_t)got;
    }
    fat16_close(&vol, &f);
    check(total == 70000, "MEDIA.BIN: прочитано 70000 байт");
    check(!bad, "MEDIA.BIN: все байты совпали с ожидаемыми (35 кластеров)");

    /* файл в подкаталоге */
    n = read_file("/DIR1/NESTED.TXT", buf, sizeof(buf));
    check(n == 7 && buf[0] == 'n' && buf[5] == 'd',
          "DIR1/NESTED.TXT читается из подкаталога");

    /* отсутствующий файл */
    check(fat16_open(&vol, "/NOPE.TXT", FAT16_O_RDONLY, &f) == FAT16_ERR_NOENT,
          "несуществующий файл -> FAT16_ERR_NOENT");
    /* каталог вместо файла */
    check(fat16_open(&vol, "/DIR1", FAT16_O_RDONLY, &f) == FAT16_ERR_ISDIR,
          "открытие каталога как файла -> FAT16_ERR_ISDIR");
    check(fat16_open(&vol, "BAD*NAME.TXT", FAT16_O_RDONLY, &f) == FAT16_ERR_INVAL,
          "недопустимое имя -> FAT16_ERR_INVAL");
}

static void test_readdir(void)
{
    out("\nПеребор каталога:\n");

    fat16_dirent_t de;
    int count = 0;
    int saw_hello = 0, saw_dir1 = 0, saw_volume_label = 0;

    while (fat16_readdir(&vol, "/", (uint32_t)count, &de) == 1) {
        if (de.name[0] == 'H')
            saw_hello = 1;
        if (de.name[0] == 'D' && de.attr & FAT16_ATTR_DIRECTORY)
            saw_dir1 = 1;
        if (de.attr & FAT16_ATTR_VOLUME_ID)
            saw_volume_label = 1;
        count++;
    }
    check(count >= 4, "в корне не меньше 4 записей");
    check(saw_hello, "в корне есть HELLO.TXT");
    check(saw_dir1, "в корне есть каталог DIR1");
    check(!saw_volume_label, "метка тома не показывается в readdir()");

    count = 0;
    while (fat16_readdir(&vol, "/DIR1", (uint32_t)count, &de) == 1)
        count++;
    check(count == 1, "в DIR1 ровно одна запись (NESTED.TXT)");
    check(count == 1 && fat16_readdir(&vol, "/DIR1", 0, &de) == 1 &&
          de.name[0] == 'N', "запись в DIR1 называется NESTED.TXT");
}

static int text_len;            /* длина строки, записанной в NEW.TXT */

static void test_write_new(void)
{
    out("\nСоздание и запись файлов:\n");

    fat16_file_t f;

    /* новый файл в корне */
    check(fat16_open(&vol, "/NEW.TXT", FAT16_O_WRONLY | FAT16_O_CREAT, &f) == FAT16_OK,
          "создание NEW.TXT");
    const char *text = "written by ex-nihilo-OS fat16 driver\n";
    int len = 0;

    while (text[len])
        len++;
    text_len = len;
    check(fat16_write(&vol, &f, text, (uint32_t)len) == len, "запись строки");
    check(fat16_close(&vol, &f) == FAT16_OK, "закрытие NEW.TXT");
    check(fat16_exists(&vol, "/NEW.TXT") == 1, "NEW.TXT виден в корне");

    /* перечитать своим же драйвером */
    static unsigned char buf[512];
    int n = read_file("/NEW.TXT", buf, sizeof(buf));

    check(n == len, "перечитанная длина совпала");
    int same = (n == len);

    for (int i = 0; i < n && same; i++)
        if (buf[i] != (unsigned char)text[i])
            same = 0;
    check(same, "перечитанное содержимое совпало");

    /* дописывание в конец */
    check(fat16_open(&vol, "/NEW.TXT", FAT16_O_WRONLY | FAT16_O_APPEND, &f) == FAT16_OK,
          "открытие на дописывание");
    check(fat16_write(&vol, &f, "tail", 4) == 4, "дописано 4 байта");
    fat16_close(&vol, &f);

    n = read_file("/NEW.TXT", buf, sizeof(buf));
    check(n == len + 4, "длина после дописывания выросла на 4");

    /* перезапись со смещением */
    check(fat16_open(&vol, "/NEW.TXT", FAT16_O_RDWR, &f) == FAT16_OK, "открытие RDWR");
    check(fat16_seek(&vol, &f, 0, 0) == 0, "seek в начало");
    check(fat16_write(&vol, &f, "WRITTEN", 7) == 7, "перезапись 7 байт");
    fat16_close(&vol, &f);

    n = read_file("/NEW.TXT", buf, sizeof(buf));
    check(n == len + 4 && buf[0] == 'W' && buf[7] == ' ',
          "перезапись не затронула остаток файла");

    /* большой файл: больше кластера, с проверкой на стыке */
    check(fat16_open(&vol, "/BIG2.BIN", FAT16_O_WRONLY | FAT16_O_CREAT, &f) == FAT16_OK,
          "создание BIG2.BIN");
    static unsigned char big[9000];
    for (unsigned int i = 0; i < sizeof(big); i++)
        big[i] = (unsigned char)((i * 31 + 7) & 0xFF);
    check(fat16_write(&vol, &f, big, sizeof(big)) == (int)sizeof(big),
          "запись 9000 байт (5 кластеров)");
    fat16_close(&vol, &f);

    static unsigned char back[9000];
    n = read_file("/BIG2.BIN", back, sizeof(back));
    check(n == (int)sizeof(big), "BIG2.BIN: длина 9000");
    same = (n == (int)sizeof(big));
    for (int i = 0; i < n && same; i++)
        if (back[i] != big[i])
            same = 0;
    check(same, "BIG2.BIN: все 9000 байт совпали");

    /* усечение */
    check(fat16_open(&vol, "/BIG2.BIN", FAT16_O_RDWR, &f) == FAT16_OK, "открытие на усечение");
    check(fat16_truncate(&vol, &f, 100) == FAT16_OK, "truncate до 100");
    fat16_close(&vol, &f);
    n = read_file("/BIG2.BIN", back, sizeof(back));
    check(n == 100, "после truncate длина 100");

    /* файл в подкаталоге */
    check(fat16_open(&vol, "/DIR1/SUB.TXT", FAT16_O_WRONLY | FAT16_O_CREAT, &f) == FAT16_OK,
          "создание DIR1/SUB.TXT");
    check(fat16_write(&vol, &f, "sub", 3) == 3, "запись в подкаталог");
    fat16_close(&vol, &f);
    n = read_file("/DIR1/SUB.TXT", buf, sizeof(buf));
    check(n == 3, "чтение из подкаталога после записи");
}

static void test_dirs_and_unlink(void)
{
    out("\nКаталоги и удаление:\n");

    check(fat16_mkdir(&vol, "/NEWDIR") == FAT16_OK, "mkdir NEWDIR");
    check(fat16_mkdir(&vol, "/NEWDIR") == FAT16_ERR_EXIST, "повторный mkdir -> ERR_EXIST");
    check(fat16_exists(&vol, "/NEWDIR") == 1, "NEWDIR существует");
    check(fat16_rmdir(&vol, "/NEWDIR") == FAT16_OK, "rmdir пустого каталога");

    /* каталог с файлом удалить нельзя */
    fat16_file_t f;
    check(fat16_mkdir(&vol, "/FULLDIR") == FAT16_OK, "mkdir FULLDIR");
    check(fat16_open(&vol, "/FULLDIR/A.TXT", FAT16_O_WRONLY | FAT16_O_CREAT, &f) == FAT16_OK,
          "создание файла в FULLDIR");
    fat16_close(&vol, &f);
    check(fat16_rmdir(&vol, "/FULLDIR") == FAT16_ERR_NOTEMPTY,
          "rmdir непустого каталога -> ERR_NOTEMPTY");
    check(fat16_unlink(&vol, "/FULLDIR/A.TXT") == FAT16_OK, "unlink файла");
    check(fat16_rmdir(&vol, "/FULLDIR") == FAT16_OK, "после unlink каталог удаляется");

    /* удаление освобождает кластеры */
    uint32_t before = fat16_free_clusters(&vol);

    check(fat16_unlink(&vol, "/BIG2.BIN") == FAT16_OK, "unlink BIG2.BIN");
    uint32_t after = fat16_free_clusters(&vol);

    check(after > before, "после unlink освободились кластеры");
    check(fat16_exists(&vol, "/BIG2.BIN") == 0, "BIG2.BIN больше нет");

    /* переименование */
    check(fat16_rename(&vol, "/NEW.TXT", "/RENAMED.TXT") == FAT16_OK, "rename NEW->RENAMED");
    check(fat16_exists(&vol, "/RENAMED.TXT") == 1, "RENAMED.TXT существует");
    check(fat16_exists(&vol, "/NEW.TXT") == 0, "старого имени нет");

    static unsigned char buf[512];
    int n = read_file("/RENAMED.TXT", buf, sizeof(buf));

    check(n == text_len + 4, "содержимое после rename не потерялось");
    check(n == text_len + 4 && buf[0] == 'W' && buf[n - 1] == 'l',
          "после rename читаются и перезапись, и дописанный хвост");

    check(fat16_unlink(&vol, "/RENAMED.TXT") == FAT16_OK, "unlink RENAMED.TXT");
    check(fat16_unlink(&vol, "/NOPE.TXT") == FAT16_ERR_NOENT,
          "unlink несуществующего -> ERR_NOENT");
}

static void test_errors(void)
{
    out("\nОбработка ошибок:\n");

    fat16_file_t f;

    check(fat16_open(&vol, "/X.TXT", FAT16_O_RDONLY, &f) == FAT16_ERR_NOENT,
          "open без O_CREAT не создаёт файл");
    check(fat16_read(&vol, 0, 0, 0) == FAT16_ERR_INVAL, "read(NULL) -> INVAL");
    check(fat16_write(&vol, 0, 0, 0) == FAT16_ERR_INVAL, "write(NULL) -> INVAL");
    check(fat16_close(&vol, 0) == FAT16_ERR_INVAL, "close(NULL) -> INVAL");
    check(fat16_seek(&vol, 0, 0, 0) == FAT16_ERR_INVAL, "seek(NULL) -> INVAL");
    check(fat16_seek(&vol, &f, 0, 9) == FAT16_ERR_INVAL, "seek с плохим whence -> INVAL");
    check(fat16_readdir(&vol, "/NOPE", 0, 0) == FAT16_ERR_INVAL, "readdir(out=NULL) -> INVAL");

    /* чтение за концом файла даёт 0, а не ошибку */
    check(fat16_open(&vol, "/HELLO.TXT", FAT16_O_RDONLY, &f) == FAT16_OK, "открытие HELLO.TXT");
    check(fat16_seek(&vol, &f, 100, 0) == 100, "seek за конец файла");
    check(fat16_read(&vol, &f, &f, 1) == 0, "read за концом даёт 0");
    fat16_close(&vol, &f);

    /* запись на диск только для чтения */
    blockdev_t ro = dev;
    ro.write = 0;

    fat16_volume_t rov;
    check(fat16_mount(&ro, FAT_BASE_LBA, &rov) == FAT16_OK, "монтирование read-only");
    check(rov.read_only == 1, "том помечен read-only");
    check(fat16_open(&rov, "/HELLO.TXT", FAT16_O_WRONLY, &f) == FAT16_ERR_ROFS,
          "запись на read-only том -> ERR_ROFS");
    check(fat16_open(&rov, "/NEW2.TXT", FAT16_O_WRONLY | FAT16_O_CREAT, &f) == FAT16_ERR_ROFS,
          "создание на read-only том -> ERR_ROFS");
    check(fat16_unlink(&rov, "/HELLO.TXT") == FAT16_ERR_ROFS,
          "unlink на read-only том -> ERR_ROFS");
}

int test_main(uint32_t argc, char **argv);

int test_main(uint32_t argc, char **argv)
{
    if (argc < 2) {
        out("usage: fat_test <image>\n");
        return 2;
    }

    int fd = (int)sys_open(argv[1], O_RDWR);

    if (fd < 0) {
        out("fat_test: не открыть образ\n");
        return 2;
    }

    filedev_t ctx;
    ctx.fd = fd;

    dev.read = fd_read;
    dev.write = fd_write;
    dev.ctx = &ctx;
    dev.sectors = 32768;

    out("=== FAT16 driver tests ===\n\n");
    test_mount();
    test_read_seeded();
    test_readdir();
    test_write_new();
    test_dirs_and_unlink();
    test_errors();

    sys_close(fd);

    out("\n");
    if (failures) {
        out("FAT16 tests: FAILED, провалов ");
        out_dec((long)failures);
        out(" из ");
        out_dec((long)checks);
        out("\n");
        return 1;
    }
    out("FAT16 tests: все ");
    out_dec((long)checks);
    out(" проверок пройдены\n");
    return 0;
}
