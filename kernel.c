#include <stdint.h>
#include <stddef.h>
#include "hal/pmm.h"
#include "hal/video.h"
#include "hal/serial.h"
#include "hal/ata.h"
#include "fs/fat16.h"
#include "kernel/kapi.h"
#include "kernel/syscall.h"
#include "kernel/user.h"
#include "kernel/exec.h"
#include "kernel/paging.h"

/* Регион user-кода внутри ядра (linker.ld): его страницы paging
 * помечает User, всё остальное ядро остаётся Supervisor. */
extern uint8_t user_region_start[];
extern uint8_t user_region_end[];

/* Вывод идёт через hal/video.c: в графическом режиме VBE 1024x768
 * со шрифтом 8x16 это 128x48 символов, в текстовом — 80x25.
 * Раньше здесь был жёстко зашит буфер 0xB8000: в 80x25 отчёт не
 * помещался и верх прокручивался. */

#define COLS video_cols()
#define ROWS video_rows()

/* Раскладка в две колонки.
 *
 * Отчёт занимает ~76 строк, а в 1024x768 при шрифте 8x16 строк только
 * 48 — верх всё равно уезжал бы при прокрутке. Зато ширина экрана 128
 * символов: две колонки по 64 дают 96 видимых строк, и отчёт виден
 * целиком. В текстовом режиме 80x25 ширина не позволяет делить экран,
 * поэтому там остаётся одна колонка и обычная прокрутка.
 *
 * page_x — левый столбец текущей колонки (0 или page_w), в символах.
 * page_w — ширина колонки в символах. */
#define TWO_COLUMN_MIN_COLS  96

static unsigned int page_x;
static unsigned int page_w;

static void layout_init(void)
{
    if (COLS >= TWO_COLUMN_MIN_COLS) {
        page_w = COLS / 2;
        page_x = 0;
    } else {
        page_w = COLS;
        page_x = 0;
    }
}

/* Нарисовать один символ заданным цветом (+ дублирование в COM1
 * без цвета: серийный лог остаётся обычным текстом для парсинга). */
static void draw_char(unsigned int row, unsigned int col, char c, uint32_t fg)
{
    video_putc_color(row, page_x + col, c, fg);
    serial_putc(c);
}

/* Статус-маркеры в квадратных скобках красятся целиком, вместе со
 * скобками: "[ok...]" — зелёным, "[BUG]" — красным. Цвет известен
 * только по закрывающей скобке, поэтому текст от '[' буферизуем:
 *   - началось с "ok"  -> VIDEO_FG_GREEN
 *   - равно "BUG"      -> VIDEO_FG_RED
 *   - всё остальное (например, индекс "[12]" в карте E820) — обычным
 * Если скобка не закрылась за BRACKET_MAX символов — печатаем как есть. */
#define BRACKET_MAX 24
static char bracket_buf[BRACKET_MAX];
static unsigned int bracket_len;
static unsigned int bracket_col;
static int bracket_open;

static uint32_t bracket_color(void)
{
    if (bracket_len >= 2 && bracket_buf[0] == 'o' && bracket_buf[1] == 'k')
        return VIDEO_FG_GREEN;
    if (bracket_len == 3 && bracket_buf[0] == 'B' && bracket_buf[1] == 'U' &&
        bracket_buf[2] == 'G')
        return VIDEO_FG_RED;
    return VIDEO_FG;
}

/* Вывести накопленный внутренний текст скобок ('[' уже на экране),
 * вернуть следующую колонку. */
static unsigned int bracket_flush(unsigned int row, uint32_t fg)
{
    unsigned int len = bracket_len;

    for (unsigned int i = 0; i < len; i++)
        draw_char(row, bracket_col + 1 + i, bracket_buf[i], fg);
    bracket_open = 0;
    bracket_len = 0;
    return bracket_col + 1 + len;
}

/* Отрисовать один символ в ячейку (row, col) с учётом раскраски
 * статус-скобок выше. Возвращает следующую колонку. Вызывается
 * только из flush_line(): напрямую сюда текст больше не ходит. */
static unsigned int render_char(unsigned int row, unsigned int col, char c)
{
    if (row >= ROWS)
        return col;

    /* Внутри "[...]": копим, пока не увидим ']' или не переполнимся.
     * Колонка при этом стоит: реальный вывод сделает bracket_flush. */
    if (bracket_open) {
        if (c == ']') {
            /* Цвет наконец известен: перекрашиваем '[' и всё внутри. */
            uint32_t fg = bracket_color();
            unsigned int len = bracket_len;

            /* '[' в COM1 уже ушла при первой отрисовке — повторно
             * только в видео, иначе в логе будет "[[ok]". */
            video_putc_color(row, page_x + bracket_col, '[', fg);
            for (unsigned int i = 0; i < len; i++)
                draw_char(row, bracket_col + 1 + i, bracket_buf[i], fg);
            draw_char(row, bracket_col + 1 + len, ']', fg);
            bracket_open = 0;
            bracket_len = 0;
            return bracket_col + len + 2;
        }
        if (bracket_len < BRACKET_MAX) {
            bracket_buf[bracket_len++] = c;
            return col;
        }
        col = bracket_flush(row, VIDEO_FG);     /* не статус: как есть */
    }

    if (col >= page_w)
        return col;                     /* за пределами колонки: пропускаем */

    if (c == '[') {
        bracket_open = 1;
        bracket_len = 0;
        bracket_col = col;
        draw_char(row, col, '[', VIDEO_FG);     /* перекрасится по ']' */
        return col + 1;
    }

    draw_char(row, col, c, VIDEO_FG);
    return col + 1;
}

/* ---------------------------------------------------------------
 * Фильтр строк: на экран (и в COM1) попадают ТОЛЬКО строки, в
 * которых есть квадратная скобка, — статусы "[ok]"/"[BUG]" и
 * записи E820 "[i] ...". Весь остальной технический текст отчёта
 * (заголовки, размеры, счётчики, листинги) подавляется.
 *
 * Реализация: put_char() не рисует, а собирает строку в буфер;
 * newline() печатает её через render_char() (с раскраской скобок),
 * только если в ней встретилась '['. Подачи с явной колонкой
 * (out(..., 2, text)) при этом становятся последовательными —
 * все такие места и так продолжают строку с текущей позиции.
 * --------------------------------------------------------------- */
#define LINE_MAX 256
static char line_buf[LINE_MAX];
static unsigned int line_len;
static int line_has_bracket;

static unsigned int put_char(unsigned int row, unsigned int col, char c)
{
    (void)row;
    if (line_len < LINE_MAX) {
        line_buf[line_len++] = c;
        if (c == '[')
            line_has_bracket = 1;
    }
    return col + 1;
}

static unsigned int cursor_row;       /* определение — ниже, у newline() */

/* Напечатать собранную строку на текущей строке экрана. */
static void flush_line(void)
{
    unsigned int col = 0;

    for (unsigned int i = 0; i < line_len; i++)
        col = render_char(cursor_row, col, line_buf[i]);
    if (bracket_open)                   /* незакрытая '[' — как есть */
        bracket_flush(cursor_row, VIDEO_FG);
}

/* Прокрутка экрана на одну строку вверх.
 *   Нужна, только если отчёт не влез целиком (в текстовом режиме). */
static void scroll(void)
{
    video_scroll();
}

/* Очищает весь экран */
static void clear_screen(void)
{
    video_clear();
}

/* Выводит строку str, начиная с позиции (row, col).
 *   Возвращает колонку, следующую за последним символом. */
static unsigned int out(unsigned int row, unsigned int col, const char *str)
{
    while (*str != '\0')
        col = put_char(row, col, *str++);
    return col;
}

/* Выводит беззнаковое число в виде 0x + 8 шестнадцатеричных цифр.
 *   Возвращает колонку, следующую за последней цифрой. */
static unsigned int out_hex(unsigned int row, unsigned int col, uintptr_t value)
{
    static const char digits[] = "0123456789ABCDEF";

    col = out(row, col, "0x");
    for (int shift = 28; shift >= 0; shift -= 4)
        col = put_char(row, col, digits[(value >> shift) & 0xF]);
    return col;
}

/* Выводит беззнаковое число в десятичной записи, начиная с (row, col).
 *   Возвращает колонку, следующую за последней цифрой.
 *   Локальный массив здесь не используем: с защитой стека (по умолчанию в Ubuntu)
 *   gcc вставит вызов __stack_chk_fail, которого в ядре нет. */
static unsigned int out_dec(unsigned int row, unsigned int col, uintptr_t value)
{
    uintptr_t divisor = 1;

    /* Находим старший разряд: делитель, при котором value / divisor даёт первую цифру */
    while (value / divisor >= 10)
        divisor *= 10;

    /* Выводим цифры слева направо */
    for (; divisor > 0; divisor /= 10)
        col = put_char(row, col, (char)('0' + (value / divisor) % 10));

    return col;
}

/* Текущая строка вывода: все сообщения идут вниз, экран прокручивается */
static unsigned int cursor_row;

/* Начать новую строку, прокрутив экран при необходимости.
 * Собранная строка печатается, только если в ней есть '[' (см.
 * фильтр выше); остальные строки не занимают места на экране,
 * не уходят в COM1 и не двигают курсор. */
static void newline(void)
{
    int show = line_has_bracket;

    if (show) {
        flush_line();
        serial_putc('\n');
    }
    line_len = 0;
    line_has_bracket = 0;
    if (!show)
        return;

    cursor_row++;
    if (cursor_row < ROWS)
        return;

    /* Колонка заполнена: сначала пробуем вторую, прокручиваем только
     * когда заполнены обе. */
    if (page_w < COLS && page_x + page_w < COLS) {
        page_x += page_w;
        cursor_row = 0;
        return;
    }

    scroll();
    cursor_row = ROWS - 1;
}

/* Вывести строку целиком (с нуля), затем перейти на следующую */
static void emit(const char *str)
{
    out(cursor_row, 0, str);
    newline();
}

/* "prefix<dec>suffix" одной строкой */
static void emit_dec(const char *prefix, uintptr_t value, const char *suffix)
{
    unsigned int col = out(cursor_row, 0, prefix);
    col = out_dec(cursor_row, col, value);
    out(cursor_row, col, suffix);
    newline();
}

/* "prefix<hex>suffix" одной строкой */
static void emit_hex(const char *prefix, uintptr_t value, const char *suffix)
{
    unsigned int col = out(cursor_row, 0, prefix);
    col = out_hex(cursor_row, col, value);
    out(cursor_row, col, suffix);
    newline();
}

/* Одна запись карты E820 с полным разбором:
 *   [i] base=0x........ len=0x........ type=N free|used  pages=N */
static void emit_entry(uint32_t index, const e820_entry_t *e)
{
    unsigned int col;

    col = out(cursor_row, 0, "[");
    col = out_dec(cursor_row, col, index);
    col = out(cursor_row, col, "] base=");
    col = out_hex(cursor_row, col, (uintptr_t)e->base);
    col = out(cursor_row, col, " len=");
    col = out_hex(cursor_row, col, (uintptr_t)e->length);
    col = out(cursor_row, col, " type=");
    col = out_dec(cursor_row, col, e->type);

    /* сколько страниц по 4 КиБ приносит эта запись */
    col = out(cursor_row, col, (e->type == 1) ? " free pages=" : " used pages=");
    out_dec(cursor_row, col, (uintptr_t)(e->length / PAGE_SIZE));

    newline();
}

/* Обработчик вывода для kprint(): Ring 3 печатает через SYS_KPRINT,
 * а рисует уже Ring 0 — этой функцией, в общую раскладку экрана. */
static unsigned int print_col;
static void syscall_print(const char *str)
{
    for (; *str; str++) {
        serial_putc(*str);
        if (*str == '\n') {
            print_col = 0;
            cursor_row++;
            if (cursor_row >= ROWS) {
                video_scroll();
                cursor_row = ROWS - 1;
            }
        } else if (*str == '\b') {
            if (print_col > 0) {
                print_col--;
                video_putc(cursor_row, print_col, ' ');
            }
        } else {
            if (print_col < COLS) {
                video_putc(cursor_row, print_col, *str);
                print_col++;
            }
        }
    }
}

void syscall_clear_screen(void)
{
    clear_screen();
    cursor_row = 0;
    print_col = 0;
}

void kmain(void)
{
    serial_init();
    pmm_init();

    int gfx = video_init();

    clear_screen();
    layout_init();
    cursor_row = 0;

    /* ================= 0. Видеорежим ================= */
    emit("=== ex-nihilo-OS: physical memory report ===");
    emit(gfx ? "Video: VBE linear framebuffer, 32/16/24 bpp, 8x16 ROM font"
             : "Video: VGA text mode 80x25 (VBE unavailable)");
    emit_dec("Screen: ", video_width(), (gfx ? " px wide" : " (text cols)"));
    emit_dec("Height: ", video_height(), (gfx ? " px" : " (text rows)"));
    emit_dec("Characters per screen: ", COLS, " columns");
    emit_dec("Lines per screen:      ", ROWS, "");
    if (page_w < COLS)
        emit_dec("Text layout: two columns of ", page_w,
                 " chars = 2x the lines on screen");
    emit("");

    /* ================= 1. Карта памяти от BIOS ================= */
    emit("--- E820 memory map: BIOS INT 15h, AX=E820h ---");
    emit_dec("E820 entries: ", pmm_e820_entries(),
             " (regions reported by firmware)");
    emit_dec("  of them usable (type=1): ", pmm_e820_usable(), "");

    uint32_t entries = pmm_e820_entries();
    for (uint32_t i = 0; i < entries; i++) {
        e820_entry_t e;
        if (pmm_e820_entry(i, &e))
            emit_entry(i, &e);
    }

    emit("");

    /* ================= 2. Арифметика по страницам ================= */
    emit("--- 4 KiB page accounting ---");
    emit_dec("Usable pages from E820 (type=1): ", pmm_usable_pages(),
             " x 4096 B");
    emit_dec("  = total usable RAM, KiB:        ", pmm_usable_bytes() / 1024, "");
    emit_dec("Pages reserved for kernel image:  ", pmm_kernel_pages(),
             " x 4096 B");
    emit_dec("Pages free for allocation:        ", pmm_free_pages(),
             " x 4096 B");
    emit_dec("  = free RAM, KiB:                ", pmm_free_pages() * 4, "");
    emit_dec("Highest page index in model:      ", pmm_total_pages() - 1, "");
    emit_hex("Top address covered by bitmap:    ", pmm_total_pages() * PAGE_SIZE, "");
    emit("");

    /* ================= 3. Демонстрация alloc/free ================= */
    emit("--- pmm_alloc() demo: 10 pages ---");

    void *pages[10];
    for (unsigned int i = 0; i < 10; i++) {
        pages[i] = pmm_alloc();

        unsigned int col = out(cursor_row, 0, "Page ");
        col = out_dec(cursor_row, col, i);
        col = out(cursor_row, col, ": ");

        if (pages[i] == NULL) {
            out(cursor_row, col, "OUT OF MEMORY!");
        } else {
            col = out_hex(cursor_row, col, (uintptr_t)pages[i]);
            col = out(cursor_row, col, " (index ");
            col = out_dec(cursor_row, col, (uintptr_t)pages[i] / PAGE_SIZE);
            out(cursor_row, col, ")");
        }
        newline();
    }
    emit_dec("Free pages after 10 allocations:   ", pmm_free_pages(), "");
    emit("");

    /* ================= 4. Проверка pmm_free() ================= */
    emit("--- pmm_free() behaviour ---");

    uint32_t before = pmm_free_pages();
    pmm_free(pages[0]);
    emit_dec("After free(page0) free pages:      ", pmm_free_pages(),
             (pmm_free_pages() == before + 1) ? "  [ok +1]" : "  [BUG]");

    emit_dec("Free pages now:                   ", pmm_free_pages(), "");

    /* повторное освобождение и мусорные аргументы */
    uint32_t guard = pmm_free_pages();
    pmm_free(pages[0]);                 /* двойное освобождение */
    pmm_free(NULL);                     /* NULL */
    pmm_free((void *)0x1234);           /* невыровненный адрес */
    emit_dec("After double free/NULL/unaligned: ", pmm_free_pages(),
             (pmm_free_pages() == guard) ? "  [ok, unchanged]" : "  [BUG]");

    /* попытка освободить зарезервированное */
    guard = pmm_free_pages();
    pmm_free((void *)0xB8000);          /* видеопамять VGA / LFB-окно */
    pmm_free((void *)0x10000);          /* код ядра */
    pmm_free((void *)0xF0000);          /* BIOS ROM */
    emit_dec("After freeing VGA/kernel/ROM:     ", pmm_free_pages(),
             (pmm_free_pages() == guard) ? "  [ok, protected]" : "  [BUG]");
    emit("");

    /* ================= 5. Файловая система FAT16 ================= */
    emit("=== FAT16 filesystem ===");

    static blockdev_t disk;
    static fat16_volume_t vol;

    if (ata_blockdev(&disk) != 0) {
        emit("ATA: disk not found - filesystem unavailable.");
        emit("(boot the image as a hard disk, not as a floppy)");
    } else {
        emit_dec("ATA: sectors on disk:              ", disk.sectors, "");
        emit_dec("ATA: FAT16 starts at sector:       ", EXNIHILO_FAT_LBA, "");

        int rc = fat16_mount(&disk, EXNIHILO_FAT_LBA, &vol);

        if (rc != FAT16_OK) {
            emit_dec("mount failed, error ", rc, "");
        } else {
            emit_dec("Clusters total:                   ", fat16_total_clusters(&vol), "");
            emit_dec("Clusters free:                    ", fat16_free_clusters(&vol), "");
            emit_dec("Cluster size, bytes:              ", vol.cluster_bytes, "");

            /* --- листинг корневого каталога --- */
            emit("--- directory / ---");

            fat16_dirent_t de;

            for (uint32_t i = 0; fat16_readdir(&vol, "/", i, &de) == 1; i++) {
                unsigned int col = out(cursor_row, 0, "  ");

                col = out(cursor_row, col, de.name);

                for (unsigned int pad = 12; pad > 0 && col < 12 + 4; pad--)
                    col = put_char(cursor_row, col, ' ');

                col = out(cursor_row, col, (de.attr & FAT16_ATTR_DIRECTORY)
                                            ? "<DIR>   " : "        ");
                col = out_dec(cursor_row, col, de.size);
                out(cursor_row, col, " bytes");
                newline();
            }

            /* --- чтение файла через драйвер --- */
            emit("--- read /HELLO.TXT ---");

            fat16_file_t f;

            rc = fat16_open(&vol, "/HELLO.TXT", FAT16_O_RDONLY, &f);

            if (rc != FAT16_OK) {
                emit_dec("open failed, error ", rc, "");
            } else {
                static char text[256];
                int n = fat16_read(&vol, &f, text, sizeof(text) - 1);

                fat16_close(&vol, &f);

                if (n < 0) {
                    emit_dec("read failed, error ", n, "");
                } else {
                    text[n] = '\0';
                    emit_dec("read bytes: ", n, "");
                    emit("content:");
                    out(cursor_row, 0, "  ");
                    out(cursor_row, 2, text);
                    newline();
                }
            }

            /* --- запись: проверяем, что диск действительно пишется --- */
            emit("--- write test: /WRITE.TXT ---");

            const char *payload = "written by ex-nihilo-OS\n";
            uint32_t payload_len = 0;

            while (payload[payload_len])
                payload_len++;

            uint32_t free_before = fat16_free_clusters(&vol);

            rc = fat16_open(&vol, "/WRITE.TXT",
                            FAT16_O_WRONLY | FAT16_O_CREAT | FAT16_O_TRUNC, &f);

            if (rc != FAT16_OK) {
                emit_dec("create failed, error ", rc, "");
            } else {
                int w = fat16_write(&vol, &f, payload, payload_len);
                int cr = fat16_close(&vol, &f);

                emit_dec("bytes written: ", w,
                         (w == (int)payload_len) ? "  [ok]" : "  [BUG]");
                emit_dec("close: ", cr, (cr == FAT16_OK) ? "  [ok]" : "  [BUG]");

                /* перечитать записанное */
                rc = fat16_open(&vol, "/WRITE.TXT", FAT16_O_RDONLY, &f);

                if (rc == FAT16_OK) {
                    static char back[256];
                    int n = fat16_read(&vol, &f, back, sizeof(back) - 1);

                    fat16_close(&vol, &f);

                    int same = (n == (int)payload_len);

                    for (int i = 0; i < n && same; i++)
                        if (back[i] != payload[i])
                            same = 0;

                    emit_dec("re-read bytes: ", n, same ? "  [ok, identical]"
                                                       : "  [BUG]");
                }

                emit_dec("free clusters: before ", free_before, "");
                emit_dec("              after  ", fat16_free_clusters(&vol), "");
            }

            /* --- удаление: освобождает кластеры --- */
            emit("--- unlink /WRITE.TXT ---");

            rc = fat16_unlink(&vol, "/WRITE.TXT");

            emit_dec("unlink: ", rc, (rc == FAT16_OK) ? "  [ok]" : "  [BUG]");

            if (rc == FAT16_OK)
                emit_dec("free clusters after unlink:       ",
                         fat16_free_clusters(&vol), "");

            /* --- сколько секторов реально прошло через ATA --- */
            emit_dec("ATA sectors read:                 ", ata_read_count(), "");
            emit_dec("ATA sectors written:              ", ata_write_count(), "");

            /* проверка, что удаление действительно было: файла нет */
            emit_dec("exists(/WRITE.TXT) after unlink:  ",
                     (uint32_t)fat16_exists(&vol, "/WRITE.TXT"),
                     fat16_exists(&vol, "/WRITE.TXT") ? "  [BUG]" : "  [ok]");
        }
    }
    emit("");

    /* ================= 6. Ring 3 -> syscall -> Ring 0 ================= */
    emit("=== syscall demo: Ring3 --int 0x80--> Ring0 kopen/kread ===");

    int mrc = kmount();
    if (mrc != FAT16_OK) {
        emit_dec("kmount failed, rc ", (uintptr_t)(int32_t)mrc, " - syscall demo skipped.");
    } else {
        syscall_cpu_init();             /* GDT(user/TSS) + IDT[0x80] */
        paging_init();                  /* защита памяти: CR0.PG|WP */
        paging_map((uint32_t)(uintptr_t)user_region_start,
                   (uint32_t)(user_region_end - user_region_start), 1, 1);
        kprint_register(syscall_print);

        void *ustack = kpmm_alloc();    /* страница под стек Ring 3 */
        if (ustack == NULL) {
            emit("no page for user stack - demo skipped.");
        } else {
            paging_map((uint32_t)(uintptr_t)ustack, PAGE_SIZE, 1, 1);
            int code = enter_usermode(user_main,
                                      (char *)ustack + PAGE_SIZE);
            kpmm_free(ustack);
            kexec_cleanup();        /* страница стека последнего exec'а */

            /* Мы снова в Ring 0: enter_usermode вернулся по SYS_KEXIT. */
            emit_dec("user exit code (SYS_KEXIT):       ", code,
                     (code == 0) ? "  [ok]" : "  [BUG]");
            emit_dec("CPL seen by user code (CS & 3):   ", user_demo_state.cpl,
                     (user_demo_state.cpl == 3) ? "  [ok, Ring 3]" : "  [BUG]");
            emit_dec("kopen(\"/HELLO.TXT\") -> fd:        ", user_demo_state.open_rc,
                     (user_demo_state.open_rc >= 0) ? "  [ok]" : "  [BUG]");
            emit_dec("kread() -> bytes:                 ", user_demo_state.read_rc,
                     (user_demo_state.read_rc > 0) ? "  [ok]" : "  [BUG]");
            emit_dec("kclose() -> rc:                   ", user_demo_state.close_rc,
                     (user_demo_state.close_rc == 0) ? "  [ok]" : "  [BUG]");
            if (user_demo_state.read_rc > 0) {
                out(cursor_row, 0, "  file content via syscall: ");
                out(cursor_row, 28, user_demo_state.text);
                newline();
            }

            /* --- защита памяти: Ring 3 пытается писать в ядро --- */
            void *bstack = kpmm_alloc();
            if (bstack != NULL) {
                paging_map((uint32_t)(uintptr_t)bstack, PAGE_SIZE, 1, 1);
                int bcode = enter_usermode(user_bad_main,
                                           (char *)bstack + PAGE_SIZE);
                kpmm_free(bstack);

                emit_hex("page fault addr: ",
                         paging_last_fault_addr(), "  [PF]");
                emit_dec("ring3 write to kernel page: #PF count ",
                         paging_fault_count(),
                         (bcode == USER_EXIT_PAGEFAULT)
                             ? "  [ok, #PF blocked]" : "  [BUG]");
            }

            /* --- запуск shell (/SHELL.ELF) --- */
            void *sstack = kpmm_alloc();
            if (sstack != NULL) {
                paging_map((uint32_t)(uintptr_t)sstack, PAGE_SIZE, 1, 1);
                int scode = enter_usermode(user_shell_main,
                                           (char *)sstack + PAGE_SIZE);
                kpmm_free(sstack);
                kexec_cleanup();
                emit_dec("shell exited with code: ", scode, "");
            }
        }
    }
    emit("");

    /* ================= 7. Итог ================= */
    emit("--- final state ---");
    emit_dec("Free pages remaining:             ", pmm_free_pages(), "");
    emit_dec("Free RAM remaining, KiB:          ", pmm_free_pages() * 4, "");

    /* Всё сделано. Останавливаем процессор. */
    emit("System halted (hlt loop).");
    for (;;)
        __asm__ volatile ("hlt");
}
