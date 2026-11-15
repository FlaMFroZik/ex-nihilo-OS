/* tests/pmm_test.c — проверка логики PMM на реальном коде hal/pmm.asm
 *
 * Тест собирается как обычная 32-битная Linux-программа (см. Makefile,
 * цель `make test`). PMM не обращается к физической памяти — он лишь
 * считает номера страниц, — поэтому его можно гонять в user space.
 *
 * Карта E820 задаётся тестом вручную в тех же адресах (0x0600/0x0608),
 * куда её кладёт загрузчик. Значение kernel_end = 0x108000 приходит из
 * tests/start.asm (имитирует образ ядра размером ~32 КиБ по адресу
 * 0x10000, как в настоящей сборке).
 */

#include <stdint.h>
#include <stddef.h>

#include "hal/pmm.h"
#include "hal/spinlock.h"

/* ---- минимальный вывод без libc: только Linux-сисколлы 32-bit ---- */

#define SYS_exit   1
#define SYS_write  4

static long sys_write(int fd, const void *buf, unsigned long len)
{
    long ret;
    __asm__ volatile ("int $0x80"
                      : "=a"(ret)
                      : "a"(SYS_write), "b"(fd), "c"(buf), "d"(len)
                      : "memory");
    return ret;
}

static size_t slen(const char *s)
{
    size_t n = 0;
    while (s[n]) n++;
    return n;
}

static void eputs(const char *s)
{
    sys_write(2, s, slen(s));
}

static void eputc(char c)
{
    sys_write(2, &c, 1);
}

static void eput_dec(unsigned long v)
{
    char buf[24];
    int i = 0;
    if (v == 0) { eputc('0'); return; }
    while (v) { buf[i++] = (char)('0' + v % 10); v /= 10; }
    while (i--) eputc(buf[i]);
}

static void eput_hex(unsigned long v)
{
    char buf[10];
    for (int i = 0; i < 8; i++)
        buf[i] = "0123456789ABCDEF"[(v >> (28 - 4 * i)) & 0xF];
    sys_write(2, buf, 8);
}

/* ---- мини-фреймворк ---- */

static int failures = 0;
static int checks = 0;
static const char *current_test = "";

static void check(int cond, const char *what, unsigned long got, unsigned long want)
{
    checks++;
    if (!cond) {
        failures++;
        eputs("FAIL [");
        eputs(current_test);
        eputs("] ");
        eputs(what);
        eputs(": got 0x");
        eput_hex(got);
        eputs(" want 0x");
        eput_hex(want);
        eputs("\n");
    }
}

/* Выражения вычисляются РОВНО ОДИН РАЗ: pmm_alloc() внутри проверки
 * иначе вызывался бы дважды и «съедал» лишнюю страницу. */
#define CHECK_EQ(got, want)                                  \
    do {                                                     \
        unsigned long _g = (unsigned long)(got);             \
        unsigned long _w = (unsigned long)(want);            \
        check(_g == _w, #got, _g, _w);                       \
    } while (0)

#define CHECK_TRUE(expr)                                     \
    do {                                                     \
        unsigned long _g = (unsigned long)!!(expr);          \
        check(_g, #expr, _g, 1);                             \
    } while (0)

#define RUN(fn) do { current_test = #fn; fn(); } while (0)

/* ---- эмуляция карты E820, как её оставляет загрузчик ---- */

#define E820_MAX         20
#define E820_ENTRY_SIZE  24

extern spinlock_t test_lock;

/* kernel_end из tests/start.asm */
extern char kernel_end;

/* Хранилище «карты E820» и ячейки BIOS: в user space нет адресов
 * 0x600 и 0x413, поэтому hal/pmm.asm собирается с -D... (см. Makefile). */
extern uint32_t test_e820_count;
extern uint8_t  test_e820_buf[];
extern uint16_t test_bios_mem_kb;

#define KERNEL_END        ((uintptr_t)&kernel_end)
#define KERNEL_LAST_PAGE  ((KERNEL_END - 1) / PAGE_SIZE)
#define KERNEL_PAGES      (KERNEL_LAST_PAGE + 1)

#define STACK_FIRST_PAGE  (0x80000 / PAGE_SIZE)
#define STACK_LAST_PAGE   (0x8FFFF / PAGE_SIZE)

/* Записать запись E820 номер i, вернуть 1 при успехе */
static int e820_add(unsigned index, uint64_t base, uint64_t length, uint32_t type)
{
    if (index >= E820_MAX)
        return 0;
    volatile uint32_t *e = (volatile uint32_t *)(test_e820_buf + index * E820_ENTRY_SIZE);
    e[0] = (uint32_t)base;
    e[1] = (uint32_t)(base >> 32);
    e[2] = (uint32_t)length;
    e[3] = (uint32_t)(length >> 32);
    e[4] = type;
    e[5] = 0;
    return 1;
}

static void e820_set_count(unsigned n)
{
    test_e820_count = n;
}

/* Типичная карта с 128 МиБ ОЗУ */
static void setup_map_128m(void)
{
    e820_add(0, 0x00000000, 0x0009FC00, 1);   /* базовая память: свободна */
    e820_add(1, 0x0009FC00, 0x00000400, 2);   /* EBDA */
    e820_add(2, 0x000F0000, 0x00010000, 2);   /* BIOS ROM */
    e820_add(3, 0x00100000, 0x07F00000, 1);   /* 1 МиБ .. 128 МиБ — ОЗУ */
    e820_set_count(4);
}

/* Карта с «дырой» в памяти: ОЗУ есть до 16 МиБ и ещё кусок 64..80 МиБ */
static void setup_map_with_hole(void)
{
    e820_add(0, 0x00000000, 0x0009FC00, 1);
    e820_add(1, 0x0009FC00, 0x00000400, 2);
    e820_add(2, 0x000F0000, 0x00010000, 2);
    e820_add(3, 0x00100000, 0x00F00000, 1);   /* 1 .. 16 МиБ */
    e820_add(4, 0x01000000, 0x03000000, 2);   /* 16 .. 64 МиБ: MMIO, занято */
    e820_add(5, 0x04000000, 0x01000000, 1);   /* 64 .. 80 МиБ: ОЗУ */
    e820_set_count(6);
}

/* ---- тесты ---- */

static void test_e820_is_read(void)
{
    setup_map_128m();
    pmm_init();

    CHECK_EQ(pmm_e820_entries(), 4);
    CHECK_EQ(pmm_e820_usable(), 2);           /* записи 0 и 3 */

    e820_entry_t e;
    CHECK_EQ(pmm_e820_entry(0, &e), 1);
    CHECK_EQ(e.base, 0);
    CHECK_EQ(e.length, 0x9FC00);
    CHECK_EQ(e.type, 1);

    CHECK_EQ(pmm_e820_entry(3, &e), 1);
    CHECK_EQ(e.base, 0x00100000);
    CHECK_EQ(e.length, 0x07F00000);
    CHECK_EQ(e.type, 1);

    /* записи с таким индексом нет */
    CHECK_EQ(pmm_e820_entry(4, &e), 0);
    CHECK_EQ(pmm_e820_entry(1000, &e), 0);
}

static void test_whole_ram_is_used(void)
{
    setup_map_128m();
    pmm_init();

    /* модель покрывает всю доступную ОЗУ (128 МиБ), а не 4 МиБ */
    CHECK_EQ(pmm_usable_bytes(), 128u * 1024 * 1024);
    CHECK_EQ(pmm_total_pages(), 128u * 1024 * 1024 / PAGE_SIZE);

    /* страниц ~32 тыс., а не 1024, как было при фиксированной модели */
    CHECK_TRUE(pmm_free_pages() > 30000);

    /* Вычёрпываем всю память: последняя выданная страница должна быть
     * самой верхней страницей ОЗУ (128 МиБ / 4 КиБ - 1). При прежней
     * модели в 1024 страницы сюда было не добраться в принципе. */
    uint32_t last_index = pmm_total_pages() - 1;
    void *last = NULL;
    uint32_t allocated = 0;
    for (uint32_t i = 0; i < pmm_total_pages(); i++) {
        void *q = pmm_alloc();
        if (!q)
            break;
        last = q;
        allocated++;
    }

    CHECK_TRUE(last != NULL);
    CHECK_EQ(allocated, pmm_total_pages() - KERNEL_PAGES);
    CHECK_EQ((uintptr_t)last / PAGE_SIZE, last_index);
    CHECK_TRUE(pmm_alloc() == NULL);           /* память действительно кончилась */
}

static void test_marked_areas_never_allocated(void)
{
    setup_map_128m();
    pmm_init();

    /* Стек (0x80000..0x8FFFF) лежит внутри уже зарезервированной
     * области ядра, поэтому свободных страниц ровно столько,
     * сколько осталось за kernel_end. */
    CHECK_EQ(pmm_free_pages(), pmm_total_pages() - KERNEL_PAGES);

    /* 1) никогда не выдаём память ядра, стек и занятые области E820 */
    uint32_t limit = pmm_total_pages();
    for (uint32_t i = 0; i < limit; i++) {
        void *p = pmm_alloc();
        if (!p)
            break;
        uintptr_t page = (uintptr_t)p / PAGE_SIZE;

        CHECK_TRUE(page > KERNEL_LAST_PAGE);
        CHECK_TRUE(page < STACK_FIRST_PAGE || page > STACK_LAST_PAGE);

        /* занятые области из карты: EBDA и BIOS ROM */
        CHECK_TRUE(!((uintptr_t)p >= 0x0009FC00 && (uintptr_t)p < 0x00100000));
        CHECK_TRUE(!((uintptr_t)p >= 0x000F0000 && (uintptr_t)p < 0x00100000));
    }
}

static void test_holes_are_respected(void)
{
    setup_map_with_hole();
    pmm_init();

    /* «дыра» 16..64 МиБ помечена как занятая (type 2) — не выдаётся */
    uint32_t limit = pmm_total_pages();
    for (uint32_t i = 0; i < limit; i++) {
        void *p = pmm_alloc();
        if (!p)
            break;
        uintptr_t addr = (uintptr_t)p;
        CHECK_TRUE(!(addr >= 0x01000000 && addr < 0x04000000));
    }

    /* верхняя область 64..80 МиБ доступна */
    pmm_init();
    void *top = NULL;
    for (uint32_t i = 0; i < limit; i++) {
        void *p = pmm_alloc();
        if (!p)
            break;
        top = p;
    }
    CHECK_TRUE((uintptr_t)top >= 0x04000000);
    CHECK_TRUE((uintptr_t)top < 0x05000000);
}

static void test_alloc_aligned_and_unique(void)
{
    setup_map_128m();
    pmm_init();

    void *seen[64];
    for (int i = 0; i < 64; i++) {
        seen[i] = pmm_alloc();
        CHECK_TRUE(seen[i] != NULL);
        CHECK_EQ((uintptr_t)seen[i] & (PAGE_SIZE - 1), 0);
        for (int j = 0; j < i; j++)
            CHECK_TRUE(seen[j] != seen[i]);
    }
}

static void test_exhaustion(void)
{
    setup_map_128m();
    pmm_init();

    uint32_t total = pmm_free_pages();
    for (uint32_t i = 0; i < total; i++)
        CHECK_TRUE(pmm_alloc() != NULL);

    CHECK_EQ(pmm_free_pages(), 0);
    CHECK_TRUE(pmm_alloc() == NULL);
    CHECK_EQ(pmm_free_pages(), 0);
}

static void test_free_returns_page(void)
{
    setup_map_128m();
    pmm_init();

    uint32_t before = pmm_free_pages();
    void *p = pmm_alloc();
    CHECK_EQ(pmm_free_pages(), before - 1);

    pmm_free(p);
    CHECK_EQ(pmm_free_pages(), before);

    void *q = pmm_alloc();
    CHECK_EQ((uintptr_t)q, (uintptr_t)p);
    pmm_free(q);
}

static void test_free_rejects_bad_args(void)
{
    setup_map_128m();
    pmm_init();

    uint32_t before = pmm_free_pages();

    pmm_free(NULL);                       /* NULL */
    pmm_free((void *)0x1001);             /* не выровнен */
    pmm_free((void *)0x7FFFFFFF);         /* вне модели */
    CHECK_EQ(pmm_free_pages(), before);

    /* страница внутри ОЗУ, но никогда не выдававшаяся */
    pmm_free((void *)(50u * 1024 * 1024));
    CHECK_EQ(pmm_free_pages(), before);
}

static void test_free_rejects_reserved(void)
{
    setup_map_128m();
    pmm_init();

    uint32_t before = pmm_free_pages();

    pmm_free((void *)0x0);                /* страница 0 */
    pmm_free((void *)0x1000);             /* загрузчик */
    pmm_free((void *)0x10000);            /* начало ядра */
    pmm_free((void *)(KERNEL_LAST_PAGE * PAGE_SIZE));
    pmm_free((void *)0x80000);            /* стек */
    pmm_free((void *)0x90000);
    CHECK_EQ(pmm_free_pages(), before);

    /* двойное освобождение */
    void *p = pmm_alloc();
    pmm_free(p);
    pmm_free(p);
    CHECK_EQ(pmm_free_pages(), before);
}

static void test_no_e820_fallback(void)
{
    /* загрузчик не смог опросить BIOS: только int 12h */
    e820_set_count(0);
    test_bios_mem_kb = 640;               /* 640 КиБ */
    pmm_init();

    CHECK_EQ(pmm_e820_entries(), 0);
    CHECK_EQ(pmm_total_pages(), 640 * 1024 / PAGE_SIZE);

    /* всё равно не выдаём ядро и стек */
    for (int i = 0; i < 200; i++) {
        void *p = pmm_alloc();
        if (!p)
            break;
        uintptr_t page = (uintptr_t)p / PAGE_SIZE;
        CHECK_TRUE(page > KERNEL_LAST_PAGE);
        CHECK_TRUE(page < STACK_FIRST_PAGE || page > STACK_LAST_PAGE);
    }
}

static void test_huge_and_clamped_entries(void)
{
    /* Запись, которая заканчивается выше 4 ГиБ, должна быть обрезана,
     * а 32-битное переполнение base+length — не должно ломать подсчёт. */
    e820_add(0, 0x00000000, 0x0009FC00, 1);
    e820_add(1, 0x00100000, 0xFFFFFFFF, 1);   /* 1 МиБ + ~4 ГиБ: переполнение */
    e820_add(2, 0xFFFFFFFF, 0x00001000, 1);   /* старшие 32 бита базы != 0 */
    e820_set_count(3);
    pmm_init();

    /* учтены записи 0 и 1; запись 2 начинается за пределом 4 ГиБ,
     * поэтому не даёт ни одной страницы и в статистику не попадает */
    CHECK_EQ(pmm_e820_usable(), 2);
    /* предел модели — не больше 4 ГиБ / 4 КиБ */
    CHECK_TRUE(pmm_total_pages() <= PMM_MAX_PAGES);
    CHECK_TRUE(pmm_total_pages() > 1024);
}

static void test_zero_length_and_type2_entries(void)
{
    /* записи нулевой длины и записи type != 1 не должны освобождать память */
    e820_add(0, 0x00000000, 0, 1);            /* нулевая длина */
    e820_add(1, 0x00100000, 0x00100000, 2);   /* занятая область */
    e820_add(2, 0x00100000, 0x00100000, 1);   /* та же область, но свободная */
    e820_set_count(3);
    pmm_init();

    /* запись 0 нулевой длины ничего не даёт, считается только запись 2 */
    CHECK_EQ(pmm_e820_usable(), 1);
    CHECK_EQ(pmm_total_pages(), 2u * 1024 * 1024 / PAGE_SIZE);
    CHECK_EQ(pmm_free_pages(), pmm_total_pages() - KERNEL_PAGES);
}

static void test_more_than_max_entries(void)
{
    /* загрузчик кладёт не больше E820_MAX записей; счётчик больше
     * этого значения должен безопасно обрезаться */
    e820_add(0, 0x00100000, 0x00100000, 1);
    e820_set_count(E820_MAX + 5);
    pmm_init();
    CHECK_EQ(pmm_e820_entries(), E820_MAX);
    CHECK_TRUE(pmm_total_pages() > 0);
}

static void test_alloc_above_4gib_never(void)
{
    /* даже если BIOS отдал странную карту, адрес всегда 32-битный */
    e820_add(0, 0x00000000, 0xFFFFF000, 1);
    e820_set_count(1);
    pmm_init();

    for (int i = 0; i < 1000; i++) {
        void *p = pmm_alloc();
        if (!p)
            break;
        CHECK_TRUE((uintptr_t)p < 0xFFFFF000u);
        CHECK_EQ((uintptr_t)p & (PAGE_SIZE - 1), 0);
    }
}

static void test_stress_alloc_free(void)
{
    setup_map_128m();
    pmm_init();

    uint32_t base = pmm_free_pages();
    void *pages[128];

    for (int round = 0; round < 20; round++) {
        for (int i = 0; i < 128; i++)
            pages[i] = pmm_alloc();
        for (int i = 0; i < 128; i++)
            pmm_free(pages[i]);
        CHECK_EQ(pmm_free_pages(), base);
    }
}

static void test_reinit_is_idempotent(void)
{
    setup_map_128m();
    pmm_init();
    uint32_t first = pmm_free_pages();
    pmm_init();
    pmm_init();
    CHECK_EQ(pmm_free_pages(), first);
}

/* ---- спинлок ----
 * Тест ловит классическую ошибку в реализации: если spin_lock
 * случайно обнуляет лок вместо захвата, состояние останется нулевым. */
static void test_spinlock_basic(void)
{
    test_lock.locked = 0;

    spin_lock(&test_lock);
    CHECK_EQ(test_lock.locked, 1);

    spin_unlock(&test_lock);
    CHECK_EQ(test_lock.locked, 0);

    spin_lock(&test_lock);
    CHECK_EQ(test_lock.locked, 1);
    spin_unlock(&test_lock);
    CHECK_EQ(test_lock.locked, 0);
}

int test_main(void)
{
    RUN(test_e820_is_read);
    RUN(test_whole_ram_is_used);
    RUN(test_marked_areas_never_allocated);
    RUN(test_holes_are_respected);
    RUN(test_alloc_aligned_and_unique);
    RUN(test_exhaustion);
    RUN(test_free_returns_page);
    RUN(test_free_rejects_bad_args);
    RUN(test_free_rejects_reserved);
    RUN(test_no_e820_fallback);
    RUN(test_huge_and_clamped_entries);
    RUN(test_zero_length_and_type2_entries);
    RUN(test_more_than_max_entries);
    RUN(test_alloc_above_4gib_never);
    RUN(test_stress_alloc_free);
    RUN(test_reinit_is_idempotent);
    RUN(test_spinlock_basic);

    if (failures) {
        eputs("PMM tests: ");
        eput_dec((unsigned long)failures);
        eputs(" of ");
        eput_dec((unsigned long)checks);
        eputs(" checks FAILED\n");
        return 1;
    }

    eputs("PMM tests: all ");
    eput_dec((unsigned long)checks);
    eputs(" checks passed\n");
    return 0;
}
