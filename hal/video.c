/* hal/video.c — вывод текста в графическом режиме VBE (1024x768x32).
 *
 * Режим и шрифт подготовлены загрузчиком (boot2.asm), потому что
 * INT 10h работает только в реальном режиме. Здесь — только рисование
 * в линейный буфер по адресу из VIDEO_INFO_ADDR.
 *
 * Почему не «залить фон и дорисовать символ» на каждый символ:
 * обновление одной ячейки 8x16 = 128 пикселей, это дёшево; полная
 * перерисовка экрана 1024x768 = 786432 пикселя, а выполняется она
 * только при очистке и прокрутке.
 */

#include "hal/video.h"

/* Общая область, заполненная boot2.asm. */
/* 0x0500 занят номером загрузочного диска (BOOT_DRIVE_ADDR), поэтому
 * параметры кадра лежат после карты E820, а шрифт — за ними.
 * Адреса обязаны совпадать с boot2.asm. */
#define VIDEO_INFO_ADDR  0x0900u
#define FONT_ADDR        0x1000u        /* копия ROM-шрифта, 4096 Б */

/* Поля читаем по отдельности, через volatile-указатель на dword.
 *
 * База хранится в переменной, а не в макросе с константой: на
 * константном адресе gcc доказывает «выход за границы массива» и
 * падает с -Werror=array-bounds, хотя это просто физический адрес
 * области, заполненной загрузчиком. */
static volatile uintptr_t info_base;

static uint32_t info_field(uint32_t off)
{
    return *(volatile uint32_t *)(info_base + off);
}

/* База шрифта — тоже переменная: на константном адресе gcc снова
 * доказывает выход за границы массива и падает с -Werror=array-bounds. */
static const uint8_t *font;

/* Текстовый режим 0xB8000 (запасной вариант). */
#define TEXT_MEM   ((volatile uint8_t *)0xB8000)
#define TEXT_COLS  80
#define TEXT_ROWS  25
#define TEXT_ATTR  0x07

static int     graphics;
static unsigned int cols, rows;
static unsigned int width, height, bpp;
static uint32_t pitch;
static volatile uint8_t *lfb;

int video_init(void)
{
    uint32_t info_w, info_h, info_pitch, info_lfb, info_bpp, info_active;

    info_base = VIDEO_INFO_ADDR;
    font = (const uint8_t *)FONT_ADDR;

    info_w      = info_field(0);
    info_h      = info_field(4);
    info_pitch  = info_field(8);
    info_lfb    = info_field(12);
    info_bpp    = info_field(16);
    info_active = info_field(20);

    graphics = (info_active == 1 && info_lfb != 0 &&
                info_w >= 320 && info_h >= 200);

    if (graphics) {
        width  = info_w;
        height = info_h;
        pitch  = info_pitch ? info_pitch : info_w * 4;
        bpp    = info_bpp;
        lfb    = (volatile uint8_t *)(uintptr_t)info_lfb;
        cols   = width / VIDEO_GLYPH_W;
        rows   = height / VIDEO_GLYPH_H;
    } else {
        cols = TEXT_COLS;
        rows = TEXT_ROWS;
        width = TEXT_COLS * VIDEO_GLYPH_W;
        height = TEXT_ROWS * VIDEO_GLYPH_H;
        bpp = 4;
    }
    return graphics;
}

int video_is_graphics(void) { return graphics; }
unsigned int video_cols(void) { return cols; }
unsigned int video_rows(void) { return rows; }
unsigned int video_width(void) { return width; }
unsigned int video_height(void) { return height; }
unsigned int video_bpp(void) { return bpp; }

/* ------------------------------------------------------------------
 * Пиксели
 * ------------------------------------------------------------------ */

/* Записать пиксель. VBE с 32 битами на пиксель почти всегда даёт
 * раскладку 0x00RRGGBB (байты B,G,R,0), но порядок каналов задаётся
 * полями ModeInfoBlock. Мы просим 32bpp и пишем BGRA — именно это
 * отдают QEMU/Bochs и подавляющее большинство BIOS.
 *
 * 16bpp (RGB565) поддержан на случай, если 32bpp-режима не нашлось:
 * иначе отчёт превратился бы в мусор. */
static void put_pixel(uint32_t x, uint32_t y, uint32_t rgb)
{
    volatile uint8_t *base = lfb + (uint32_t)y * pitch;

    if (bpp == 32) {
        volatile uint32_t *p = (volatile uint32_t *)(base + x * 4u);

        *p = rgb & 0x00FFFFFFu;
    } else if (bpp == 16) {
        /* 0x00RRGGBB -> 5-6-5 */
        uint32_t r = (rgb >> 16) & 0xFF;
        uint32_t g = (rgb >> 8) & 0xFF;
        uint32_t b = rgb & 0xFF;
        uint16_t v = (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));

        *(volatile uint16_t *)(base + x * 2u) = v;
    } else if (bpp == 24) {
        base[x * 3 + 0] = (uint8_t)(rgb & 0xFF);
        base[x * 3 + 1] = (uint8_t)((rgb >> 8) & 0xFF);
        base[x * 3 + 2] = (uint8_t)((rgb >> 16) & 0xFF);
    }
}

/* Залить прямоугольник — используется для очистки и стирания символа. */
static void fill_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h,
                      uint32_t rgb)
{
    for (uint32_t row = 0; row < h; row++)
        for (uint32_t col = 0; col < w; col++)
            put_pixel(x + col, y + row, rgb);
}

/* ------------------------------------------------------------------
 * Символы
 * ------------------------------------------------------------------ */

void video_putc(unsigned int row, unsigned int col, char c)
{
    if (row >= rows || col >= cols)
        return;

    if (!graphics) {
        volatile uint8_t *cell = TEXT_MEM + (row * TEXT_COLS + col) * 2;

        cell[0] = (uint8_t)c;
        cell[1] = TEXT_ATTR;
        return;
    }

    uint32_t x = col * VIDEO_GLYPH_W;
    uint32_t y = row * VIDEO_GLYPH_H;
    const uint8_t *glyph = font + ((uint8_t)c) * VIDEO_GLYPH_H;

    /* Шрифт 8x16: каждый байт — строка, старший бит — левый пиксель. */
    for (uint32_t gy = 0; gy < VIDEO_GLYPH_H; gy++) {
        uint8_t bits = glyph[gy];

        for (uint32_t gx = 0; gx < VIDEO_GLYPH_W; gx++) {
            uint32_t on = (bits >> (7 - gx)) & 1u;

            put_pixel(x + gx, y + gy, on ? VIDEO_FG : 0);
        }
    }
}

void video_clear(void)
{
    if (!graphics) {
        for (unsigned int row = 0; row < TEXT_ROWS; row++)
            for (unsigned int col = 0; col < TEXT_COLS; col++) {
                volatile uint8_t *cell = TEXT_MEM + (row * TEXT_COLS + col) * 2;

                cell[0] = ' ';
                cell[1] = TEXT_ATTR;
            }
        return;
    }

    /* Работаем построчно: так запись идёт в память подряд и не
     * разбрасывается по страницам кадра. */
    for (uint32_t y = 0; y < height; y++) {
        volatile uint8_t *line = lfb + y * pitch;

        if (bpp == 32) {
            volatile uint32_t *p = (volatile uint32_t *)line;

            for (uint32_t x = 0; x < width; x++)
                p[x] = 0;
        } else {
            for (uint32_t x = 0; x < width; x++)
                put_pixel(x, y, 0);
        }
    }
}

void video_scroll(void)
{
    if (!graphics) {
        for (unsigned int row = 1; row < TEXT_ROWS; row++)
            for (unsigned int col = 0; col < TEXT_COLS; col++) {
                volatile uint8_t *dst = TEXT_MEM + ((row - 1) * TEXT_COLS + col) * 2;
                volatile uint8_t *src = TEXT_MEM + (row * TEXT_COLS + col) * 2;

                dst[0] = src[0];
                dst[1] = src[1];
            }
        for (unsigned int col = 0; col < TEXT_COLS; col++) {
            volatile uint8_t *cell = TEXT_MEM + ((TEXT_ROWS - 1) * TEXT_COLS + col) * 2;

            cell[0] = ' ';
            cell[1] = TEXT_ATTR;
        }
        return;
    }

    /* Сдвиг на одну текстовую строку = 16 строк пикселей. Копируем
     * построчно сверху вниз: двигаем вверх, поэтому источник ещё не
     * затёрт.
     *
     * Копируем по 32 бита (и разворачиваем по 8 слов за итерацию):
     * прокрутка трогает весь кадр — 3072 КБ, и побайтовое копирование
     * здесь обошлось бы в миллионы итераций на каждую строку. */
    uint32_t line_bytes = width * (bpp / 8);
    uint32_t words = line_bytes / 4;

    for (uint32_t y = 0; y < height - VIDEO_GLYPH_H; y++) {
        volatile uint32_t *dst = (volatile uint32_t *)(lfb + y * pitch);
        volatile const uint32_t *src =
            (volatile const uint32_t *)(lfb + (y + VIDEO_GLYPH_H) * pitch);

        uint32_t i = 0;

        for (; i + 8 <= words; i += 8) {
            dst[i + 0] = src[i + 0];
            dst[i + 1] = src[i + 1];
            dst[i + 2] = src[i + 2];
            dst[i + 3] = src[i + 3];
            dst[i + 4] = src[i + 4];
            dst[i + 5] = src[i + 5];
            dst[i + 6] = src[i + 6];
            dst[i + 7] = src[i + 7];
        }
        for (; i < words; i++)
            dst[i] = src[i];
    }

    fill_rect(0, height - VIDEO_GLYPH_H, width, VIDEO_GLYPH_H, 0);
}
