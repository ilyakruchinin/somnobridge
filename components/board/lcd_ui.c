/*
 * SomnoPal — minimal text UI on the ST7789.
 * Copyright (C) 2026 Ilya Kruchinin <https://github.com/ilyakruchinin>
 * GPLv3+ — see LICENSE and NOTICE.
 *
 * Full-screen RGB565 framebuffer in SRAM (~64 KiB) would be wasteful; we
 * render per-row into a small static buffer and push it with
 * esp_lcd_panel_draw_bitmap.
 */
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "board.h"
#include "font8x16.h"
#include "esp_lcd_panel_ops.h"
#include "esp_log.h"

#define FONT_W   8
#define FONT_H   16
#define FG       0xFFFF   /* white */
#define BG       0x0000   /* black */

extern esp_lcd_panel_handle_t board_lcd_panel(void);

static uint16_t s_rowbuf[BOARD_LCD_HRES * FONT_H];

void lcd_ui_clear(void)
{
    memset(s_rowbuf, 0, sizeof(s_rowbuf));
    for (int y = 0; y < BOARD_LCD_VRES; y += FONT_H) {
        esp_lcd_panel_draw_bitmap(board_lcd_panel(), 0, y,
                                  BOARD_LCD_HRES, y + FONT_H, s_rowbuf);
        board_lcd_wait_done();
    }
}

static void draw_glyph(int x, int row, uint8_t c, int invert)
{
    const uint8_t *g;
    if (c < 0x20 || c > 0x7E) {
        c = '?';
    }
    g = font8x16[c - 0x20];
    for (int y = 0; y < FONT_H; y++) {
        uint8_t bits = g[y];
        for (int b = 0; b < FONT_W; b++) {
            uint16_t px = (bits & (0x80 >> b)) ? FG : BG;
            if (invert) {
                px = ~px;
            }
            s_rowbuf[y * BOARD_LCD_HRES + x + b] = px;
        }
    }
}

void lcd_ui_row(int row, const char *text, int invert)
{
    if (row < 0 || row >= BOARD_LCD_ROWS) {
        return;
    }
    memset(s_rowbuf, invert ? 0xFF : 0x00, sizeof(s_rowbuf));
    int col = 0;
    while (text && *text && col < BOARD_LCD_COLS) {
        draw_glyph(col * FONT_W, row, (uint8_t)*text++, invert);
        col++;
    }
    int y = row * FONT_H;
    esp_lcd_panel_draw_bitmap(board_lcd_panel(), 0, y,
                              BOARD_LCD_HRES, y + FONT_H, s_rowbuf);
    /* draw_bitmap queues the DMA — don't let the next row rewrite
     * s_rowbuf while the SPI transfer is still reading it          */
    board_lcd_wait_done();
}

int lcd_ui_rowf(int row, int invert, const char *fmt, ...)
{
    char buf[BOARD_LCD_COLS + 1];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    lcd_ui_row(row, buf, invert);
    return n;
}

void lcd_ui_headline(const char *text)
{
    lcd_ui_row(0, text, 1);
}

void lcd_ui_footer(const char *text)
{
    lcd_ui_row(BOARD_LCD_ROWS - 1, text, 0);
}

void lcd_ui_pin_screen(const char *title, const char *code, const char *hint)
{
    lcd_ui_clear();
    lcd_ui_row(1, title, 1);
    int clen = code ? (int)strlen(code) : 0;
    int pad = (BOARD_LCD_COLS - clen) / 2;
    if (pad < 0) {
        pad = 0;
    }
    char line[BOARD_LCD_COLS + 1];
    memset(line, ' ', sizeof(line) - 1);
    line[BOARD_LCD_COLS] = '\0';
    memcpy(line + pad, code, clen > BOARD_LCD_COLS ? BOARD_LCD_COLS : clen);
    lcd_ui_row(6, line, 0);
    lcd_ui_row(13, hint, 0);
}
