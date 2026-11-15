/* tests/pmm_test.c — проверка логики PMM на реальном коде hal/pmm.asm
 *
 * Тест собирается как обычная 32-битная Linux-программа (см. tests/run.sh).
 * PMM не обращается к физической памяти: он лишь считает индексы,
 * поэтому его можно гонять в user space.
 *
 * Ожидания построены вокруг kernel_end = 0x2000 (страницы 0..1 заняты)
 * и зарезервированных страниц 160..255 (VGA + BIOS ROM).
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
    char buf[11];
    int i;
    buf[0] = '0'; buf[1] = 'x';
    for (i = 0; i < 8; i++)
        buf[2 + i] = "0123456789ABCDEF"[(v >> (28 - 4 * i)) & 0xF];
    sys_write(2, buf, 10);
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
        eputs(": got ");
        eput_hex(got);
        eputs(" want ");
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

/* индексы зарезервированных областей */
#define KERNEL_PAGES      2u                       /* 0x0 .. 0x1FFF */
#define LOW_RESERVED_LO   128u                     /* 0x80000 — стек ядра */
#define LOW_RESERVED_HI   255u                     /* 0xFFFFF — BIOS ROM */
#define RESERVED_PAGES    (KERNEL_PAGES + (LOW_RESERVED_HI - LOW_RESERVED_LO + 1))

/* ---- тесты ---- */

static void test_init_reserves_low_memory(void)
{
    pmm_init();

    uint32_t free_pages = pmm_free_pages();
    /* всего 1024; заняты: 2 (ядро) + 128 (стек, EBDA, VGA, ROM) = 130 */
    CHECK_EQ(free_pages, PMM_PAGE_COUNT - RESERVED_PAGES);

    /* первые свободные страницы идут сразу за ядром */
    void *p = pmm_alloc();
    CHECK_EQ((uintptr_t)p, KERNEL_PAGES * PAGE_SIZE);
    pmm_free(p);
}

static void test_never_returns_reserved(void)
{
    pmm_init();

    for (int i = 0; i < 2000; i++) {
        void *p = pmm_alloc();
        if (!p)
            break;
        uintptr_t page = (uintptr_t)p / PAGE_SIZE;
        CHECK_TRUE(page >= KERNEL_PAGES);
        CHECK_TRUE(page < PMM_PAGE_COUNT);
        CHECK_TRUE(page < LOW_RESERVED_LO || page > LOW_RESERVED_HI);
    }
}

static void test_alloc_is_aligned_and_unique(void)
{
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
    pmm_init();

    uint32_t expect_free = PMM_PAGE_COUNT - RESERVED_PAGES;
    for (uint32_t i = 0; i < expect_free; i++)
        CHECK_TRUE(pmm_alloc() != NULL);

    /* память кончилась */
    CHECK_EQ(pmm_free_pages(), 0);
    CHECK_TRUE(pmm_alloc() == NULL);
    CHECK_TRUE(pmm_alloc() == NULL);
    CHECK_EQ(pmm_free_pages(), 0);
}

static void test_free_returns_page(void)
{
    pmm_init();

    uint32_t before = pmm_free_pages();
    void *p = pmm_alloc();
    CHECK_EQ(pmm_free_pages(), before - 1);

    pmm_free(p);
    CHECK_EQ(pmm_free_pages(), before);

    /* та же страница снова выдаётся */
    void *q = pmm_alloc();
    CHECK_EQ((uintptr_t)q, (uintptr_t)p);
    pmm_free(q);
}

static void test_free_rejects_bad_args(void)
{
    pmm_init();

    uint32_t before = pmm_free_pages();

    pmm_free(NULL);                     /* NULL */
    CHECK_EQ(pmm_free_pages(), before);

    pmm_free((void *)0x1001);           /* не выровнен */
    CHECK_EQ(pmm_free_pages(), before);

    pmm_free((void *)0x7FFFFFFF);       /* не выровнен */
    CHECK_EQ(pmm_free_pages(), before);

    pmm_free((void *)0xFFFFF000);       /* вне модели памяти */
    CHECK_EQ(pmm_free_pages(), before);

    pmm_free((void *)0x10000000);       /* вне модели памяти */
    CHECK_EQ(pmm_free_pages(), before);
}

static void test_free_rejects_reserved_and_double_free(void)
{
    pmm_init();

    uint32_t before = pmm_free_pages();

    /* страница загрузчика/ядра */
    pmm_free((void *)0x0);
    pmm_free((void *)0x1000);
    CHECK_EQ(pmm_free_pages(), before);

    /* стек ядра, EBDA, видеопамять, BIOS ROM */
    pmm_free((void *)(LOW_RESERVED_LO * PAGE_SIZE));    /* 0x80000 */
    pmm_free((void *)0x90000);                          /* стек ядра */
    pmm_free((void *)0xB8000);                          /* VGA */
    pmm_free((void *)(LOW_RESERVED_HI * PAGE_SIZE));    /* 0xFF000 — последняя страница ROM */
    CHECK_EQ(pmm_free_pages(), before);

    /* двойное освобождение не увеличивает число свободных страниц */
    void *p = pmm_alloc();
    pmm_free(p);
    pmm_free(p);
    CHECK_EQ(pmm_free_pages(), before);
    pmm_free(p);                        /* второй раз уже нечего освобождать */
}

static void test_stress_alloc_free(void)
{
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
    for (int i = 0; i < 3; i++)
        pmm_init();
    CHECK_EQ(pmm_free_pages(), PMM_PAGE_COUNT - RESERVED_PAGES);
}

/* ---- спинлок ----
 * Тест ловит классическую ошибку в реализации: если spin_lock
 * случайно обнуляет лок вместо захвата, состояние после вызова
 * останется нулевым. */
extern spinlock_t test_lock;

static void test_spinlock_basic(void)
{
    test_lock.locked = 0;

    spin_lock(&test_lock);
    CHECK_EQ(test_lock.locked, 1);      /* лок захвачен */

    spin_unlock(&test_lock);
    CHECK_EQ(test_lock.locked, 0);      /* лок отпущен */

    /* повторный захват/отпускание работают так же */
    spin_lock(&test_lock);
    CHECK_EQ(test_lock.locked, 1);
    spin_unlock(&test_lock);
    CHECK_EQ(test_lock.locked, 0);
}

int test_main(void)
{
    RUN(test_init_reserves_low_memory);
    RUN(test_never_returns_reserved);
    RUN(test_alloc_is_aligned_and_unique);
    RUN(test_exhaustion);
    RUN(test_free_returns_page);
    RUN(test_free_rejects_bad_args);
    RUN(test_free_rejects_reserved_and_double_free);
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
