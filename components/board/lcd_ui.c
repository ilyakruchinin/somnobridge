/*
 * SomnoBridge — minimal text UI on the ST7789.
 * Copyright (C) 2026 Ilya Kruchinin <https://github.com/ilyakruchinin>
 * GPLv3+ — see LICENSE and NOTICE.
 *
 * The panel runs landscape (240x135) for every screen. Full-screen
 * RGB565 framebuffer in SRAM (~64 KiB) would be wasteful; we render
 * per-row into a small static buffer and push it with
 * esp_lcd_panel_draw_bitmap.
 */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
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

static uint16_t s_line[BOARD_LCD_HRES];   /* one landscape output line */

/* The ST7789 expects RGB565 high byte first; the SPI panel IO ships the
 * buffer as-is, so a uint16_t stored little-endian arrives swapped
 * (grey 0x94B2 renders as 0xB294 = violet). Swap at fill time — BG/FG
 * are invariant, which is why the old mono UI never showed it. */
static inline uint16_t sw16(uint16_t c)
{
    return (uint16_t)((c >> 8) | (c << 8));
}

static void blit(int x, int y, int w, const uint16_t *px)
{
    esp_lcd_panel_draw_bitmap(board_lcd_panel(), x, y, x + w, y + 1, px);
    board_lcd_wait_done();
}

void lcd_ui_rect(int x, int y, int w, int h, uint16_t color)
{
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > BOARD_LCD_HRES) w = BOARD_LCD_HRES - x;
    if (y + h > BOARD_LCD_VRES) h = BOARD_LCD_VRES - y;
    if (w <= 0 || h <= 0) {
        return;
    }
    for (int i = 0; i < w; i++) {
        s_line[i] = sw16(color);
    }
    for (int r = 0; r < h; r++) {
        blit(x, y + r, w, s_line);
    }
}

void lcd_ui_clear(void)
{
    lcd_ui_rect(0, 0, BOARD_LCD_HRES, BOARD_LCD_VRES, BG);
}

int lcd_ui_text_w(int scale, const char *text)
{
    if (!text || scale < 1) {
        return 0;
    }
    return (int)strlen(text) * FONT_W * scale;
}

static int lcd_ui_text_bg(int x, int y, int scale, uint16_t color, uint16_t bg,
                          const char *text)
{
    if (!text || !*text || scale < 1 || y >= BOARD_LCD_VRES) {
        return 0;
    }
    int w = lcd_ui_text_w(scale, text);
    if (x < 0) {
        x = 0;
    }
    if (x + w > BOARD_LCD_HRES) {
        w = BOARD_LCD_HRES - x;
    }
    if (w <= 0) {
        return 0;
    }
    for (int oy = 0; oy < FONT_H * scale; oy++) {
        for (int i = 0; i < w; i++) {
            s_line[i] = sw16(bg);
        }
        int cx = 0;
        for (const char *p = text; *p && cx < w; p++, cx += FONT_W * scale) {
            uint8_t c = (uint8_t)*p;
            const uint8_t *g =
                font8x16[(c >= 0x20 && c <= 0x7E) ? c - 0x20 : '?' - 0x20];
            uint8_t bits = g[oy / scale];
            for (int b = 0; b < FONT_W; b++) {
                if (!(bits & (0x80 >> b))) {
                    continue;
                }
                int px0 = cx + b * scale;
                for (int s = 0; s < scale && px0 + s < w; s++) {
                    s_line[px0 + s] = sw16(color);
                }
            }
        }
        blit(x, y + oy, w, s_line);
    }
    return w;
}

int lcd_ui_text(int x, int y, int scale, uint16_t color, const char *text)
{
    return lcd_ui_text_bg(x, y, scale, color, BG, text);
}

void lcd_ui_text_center(int y, int scale, uint16_t color, const char *text)
{
    int x = (BOARD_LCD_HRES - lcd_ui_text_w(scale, text)) / 2;
    lcd_ui_text(x < 0 ? 0 : x, y, scale, color, text);
}

void lcd_ui_row(int row, const char *text, int invert)
{
    if (row < 0 || row >= BOARD_LCD_ROWS) {
        return;
    }
    int y = row * FONT_H;
    if (invert) {
        lcd_ui_rect(0, y, BOARD_LCD_HRES, FONT_H, FG);
    }
    char buf[BOARD_LCD_COLS + 1];
    size_t n = text ? strlen(text) : 0;
    if (n > BOARD_LCD_COLS) {
        n = BOARD_LCD_COLS;
    }
    if (n) {
        memcpy(buf, text, n);
    }
    buf[n] = '\0';
    lcd_ui_text_bg(0, y, 1, invert ? BG : FG, invert ? FG : BG, buf);
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

void lcd_ui_wifi_bars(int x, int y, int rssi, bool connected)
{
    static const int h[4] = {4, 7, 10, 13};
    static const uint16_t off = 0x2945;   /* dark slate */
    int active = 0;
    uint16_t col = off;
    if (connected) {
        if (rssi >= -60) {
            active = 4;
            col = LCD_RGB(0, 255, 120);
        } else if (rssi >= -70) {
            active = 3;
            col = LCD_RGB(100, 255, 100);
        } else if (rssi >= -80) {
            active = 2;
            col = LCD_RGB(255, 220, 0);
        } else {
            active = 1;
            col = LCD_RGB(255, 60, 60);
        }
    }
    for (int i = 0; i < 4; i++) {
        lcd_ui_rect(x + i * 6, y + 14 - h[i], 4, h[i],
                    i < active ? col : off);
    }
}

/* Bresenham line into a 1-bit-per-pixel bitmap (row-major, w*h). */
static void bm_line(uint8_t *bm, int w, int h,
                    int x0, int y0, int x1, int y1)
{
    int dx = abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
    int dy = -abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;
    for (;;) {
        if (x0 >= 0 && x0 < w && y0 >= 0 && y0 < h) {
            bm[y0 * w + x0] = 1;
        }
        if (x0 == x1 && y0 == y1) {
            break;
        }
        int e2 = 2 * err;
        if (e2 >= dy) {
            err += dy;
            x0 += sx;
        }
        if (e2 <= dx) {
            err += dx;
            y0 += sy;
        }
    }
}

void lcd_ui_bt_rune(int x, int y, uint16_t color)
{
    enum { W = 9, H = 15 };
    uint8_t bm[W * H];
    memset(bm, 0, sizeof(bm));
    bm_line(bm, W, H, 4, 0, 4, 14);   /* stem            */
    bm_line(bm, W, H, 4, 0, 7, 3);    /* top right leg   */
    bm_line(bm, W, H, 7, 3, 1, 7);    /* upper cross     */
    bm_line(bm, W, H, 1, 7, 7, 11);   /* lower cross     */
    bm_line(bm, W, H, 7, 11, 4, 14);  /* bottom left leg */
    for (int r = 0; r < H; r++) {
        for (int c = 0; c < W; c++) {
            s_line[c] = sw16(bm[r * W + c] ? color : BG);
        }
        blit(x, y + r, W, s_line);
    }
}

void lcd_ui_sync(int x_right, int y, uint16_t color, int percent)
{
    char buf[8] = "";
    int tw = 0;
    if (percent >= 0) {
        snprintf(buf, sizeof(buf), "%d%%", percent > 100 ? 100 : percent);
        tw = lcd_ui_text_w(1, buf);
    }
    int ax = x_right - (tw ? tw + 6 : 0) - 15;
    /* up arrow: apex on top, 7-px base, 3-px shaft below */
    lcd_ui_rect(ax + 3, y + 0, 1, 1, color);
    lcd_ui_rect(ax + 2, y + 1, 3, 1, color);
    lcd_ui_rect(ax + 1, y + 2, 5, 1, color);
    lcd_ui_rect(ax + 0, y + 3, 7, 1, color);
    lcd_ui_rect(ax + 2, y + 4, 3, 6, color);
    /* down arrow: shaft on top, apex at the bottom */
    lcd_ui_rect(ax + 10, y + 0, 3, 6, color);
    lcd_ui_rect(ax + 8, y + 6, 7, 1, color);
    lcd_ui_rect(ax + 9, y + 7, 5, 1, color);
    lcd_ui_rect(ax + 10, y + 8, 3, 1, color);
    lcd_ui_rect(ax + 11, y + 9, 1, 1, color);
    if (tw) {
        lcd_ui_text(x_right - tw, y - 3, 1, color, buf);
    }
}

void lcd_ui_progress(int x, int y, int w, int h, int percent, uint16_t color)
{
    if (percent < 0) {
        percent = 0;
    }
    if (percent > 100) {
        percent = 100;
    }
    lcd_ui_rect(x, y, w, 1, color);
    lcd_ui_rect(x, y + h - 1, w, 1, color);
    lcd_ui_rect(x, y, 1, h, color);
    lcd_ui_rect(x + w - 1, y, 1, h, color);
    int inner = w - 2;
    int fill = inner * percent / 100;
    if (fill > 0) {
        lcd_ui_rect(x + 1, y + 1, fill, h - 2, color);
    }
    if (fill < inner) {
        lcd_ui_rect(x + 1 + fill, y + 1, inner - fill, h - 2, BG);
    }
}

void lcd_ui_pin_screen(const char *title, const char *code, const char *hint)
{
    lcd_ui_clear();
    if (title && *title) {
        lcd_ui_text_center(10, 1, FG, title);
    }
    /* 6 digits * 4x (32 px) = 192 px — comfortably inside 240 px */
    lcd_ui_text_center(32, 4, FG, code ? code : "------");
    if (hint && *hint) {
        lcd_ui_text_center(110, 1, 0x8C71, hint);
    }
}
