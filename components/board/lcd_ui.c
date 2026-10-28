/*
 * SomnoBridge — minimal text UI on the ST7789.
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
static uint16_t s_pinrow[BOARD_LCD_VRES];   /* one landscape output line */
static bool     s_landscape;

/* The PIN screen runs the panel in landscape (240x135); the next normal
 * row render restores portrait before drawing.                          */
static void ensure_portrait(void)
{
    if (!s_landscape) {
        return;
    }
    esp_lcd_panel_handle_t p = board_lcd_panel();
    esp_lcd_panel_swap_xy(p, false);
    esp_lcd_panel_set_gap(p, 52, 40);   /* portrait window origin      */
    s_landscape = false;
}

void lcd_ui_clear(void)
{
    ensure_portrait();
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
    ensure_portrait();
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

/* Landscape PIN screen: the pairing code is the one thing the user must
 * transcribe accurately, so it gets the whole 240-px width at 4x the
 * stock glyph size (~64 px tall digits vs the 16 px text rows used
 * everywhere else). */
#define PIN_LW       BOARD_LCD_VRES   /* landscape width  = 240         */
#define PIN_LH       BOARD_LCD_HRES   /* landscape height = 135         */
#define PIN_SCALE    4
#define PIN_TRACK    6                /* px between scaled digits       */

static void pin_fill_rows(int y0, int y1)
{
    memset(s_pinrow, 0, sizeof(s_pinrow));
    for (int y = y0; y < y1; y++) {
        esp_lcd_panel_draw_bitmap(board_lcd_panel(), 0, y,
                                  PIN_LW, y + 1, s_pinrow);
        board_lcd_wait_done();
    }
}

/* Draw `text` scaled `scale`x, centered horizontally, top edge at y0.
 * One output line at a time through s_pinrow — no framebuffer needed.  */
static void pin_text(int y0, int scale, const char *text)
{
    int len = text ? (int)strlen(text) : 0;
    if (!len) {
        return;
    }
    int total = len * FONT_W * scale + (len - 1) * PIN_TRACK;
    int x0 = (PIN_LW - total) / 2;
    if (x0 < 0) {
        x0 = 0;
    }
    for (int oy = 0; oy < FONT_H * scale; oy++) {
        memset(s_pinrow, 0, sizeof(s_pinrow));
        int x = x0;
        for (const char *p = text; *p; p++) {
            uint8_t c = (uint8_t)*p;
            const uint8_t *g =
                font8x16[(c >= 0x20 && c <= 0x7E) ? c - 0x20 : '?' - 0x20];
            uint8_t bits = g[oy / scale];
            for (int b = 0; b < FONT_W; b++) {
                if (bits & (0x80 >> b)) {
                    for (int s = 0; s < scale && x + s < PIN_LW; s++) {
                        s_pinrow[x + s] = FG;
                    }
                }
            }
            x += FONT_W * scale + PIN_TRACK;
        }
        esp_lcd_panel_draw_bitmap(board_lcd_panel(), 0, y0 + oy,
                                  PIN_LW, y0 + oy + 1, s_pinrow);
        board_lcd_wait_done();
    }
}

void lcd_ui_pin_screen(const char *title, const char *code, const char *hint)
{
    esp_lcd_panel_handle_t p = board_lcd_panel();
    esp_lcd_panel_swap_xy(p, true);
    esp_lcd_panel_set_gap(p, 40, 52);   /* landscape window origin     */
    s_landscape = true;

    pin_fill_rows(0, PIN_LH);
    if (title && *title) {
        pin_text(10, 1, title);
    }
    /* 6 digits * 4x (32 px) + 5 tracks = 222 px — nearly full width    */
    pin_text(32, PIN_SCALE, code ? code : "------");
    if (hint && *hint) {
        pin_text(110, 1, hint);
    }
}
