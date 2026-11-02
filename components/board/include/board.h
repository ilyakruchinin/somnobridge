/*
 * SomnoBridge — board support (LCD + BOOT button) for Waveshare ESP32-S3-GEEK
 * Copyright (C) 2026 Ilya Kruchinin <https://github.com/ilyakruchinin>
 *
 * This file is part of SomnoBridge.
 *
 * SomnoBridge is free software: you can redistribute it and/or modify it under
 * the terms of the GNU General Public License as published by the Free
 * Software Foundation, either version 3 of the License, or (at your option)
 * any later version.
 *
 * SomnoBridge is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program. If not, see <https://www.gnu.org/licenses/>.
 *
 * ADDITIONAL TERM (GPLv3 Section 7(b)): Redistributions must preserve the
 * attribution "SomnoBridge, originally created by Ilya Kruchinin
 * (https://github.com/ilyakruchinin)." See the NOTICE file for details.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Waveshare ESP32-S3-GEEK pin map (see docs/implementation-spec.md §2). */
#define BOARD_PIN_BOOT         0
#define BOARD_PIN_LCD_CS      10
#define BOARD_PIN_LCD_MOSI    11
#define BOARD_PIN_LCD_SCLK    12
#define BOARD_PIN_LCD_DC       8
#define BOARD_PIN_LCD_RST      9
#define BOARD_PIN_LCD_BL       7

#define BOARD_LCD_HRES       240   /* panel driven landscape: width=240    */
#define BOARD_LCD_VRES       135   /* landscape: height=135                */
#define BOARD_LCD_ROWS         8   /* 135 / 16 px font                     */
#define BOARD_LCD_COLS        30   /* 240 / 8 px font pitch (font8x16)     */

/* 24-bit RGB -> RGB565 for the primitives below. */
#define LCD_RGB(r, g, b) \
    ((uint16_t)((((r) & 0xF8) << 8) | (((g) & 0xFC) << 3) | ((b) >> 3)))

typedef enum {
    BOARD_BTN_NONE = 0,
    BOARD_BTN_SHORT,    /* 30 ms .. <1 s   */
    BOARD_BTN_LONG,     /* >=3 s hold+release (setup) */
} board_btn_event_t;

/** Init LCD (SPI + ST7789) and the BOOT button GPIO. */
esp_err_t board_init(void);

/* Backlight on/off and brightness (0..100). */
void board_backlight_set(uint8_t percent);

/**
 * Poll the BOOT button. Call from a 20 ms tick. Returns one event per
 * physical press; mid-hold >3 s state is observable via
 * board_button_held_long().
 */
board_btn_event_t board_button_poll(void);
bool board_button_held_long(void);   /* currently held >=3 s */

/* ---------------- LCD text UI ---------------- */

/* The panel runs landscape (240x135) for every screen; there is no
 * portrait mode. Text grid: BOARD_LCD_COLS x BOARD_LCD_ROWS 8x16 cells. */

/* Clear screen to black. */
void lcd_ui_clear(void);

/* Draw `text` with the 8x16 font scaled `scale`x, top-left at (x,y).
 * Returns the drawn width in px. */
int lcd_ui_text(int x, int y, int scale, uint16_t color, const char *text);
int lcd_ui_text_w(int scale, const char *text);
void lcd_ui_text_center(int y, int scale, uint16_t color, const char *text);

/* Draw one 16-px row of text (clipped to BOARD_LCD_COLS chars).
 * invert=1 draws white-on-black inversion bar (selection / alert). */
void lcd_ui_row(int row, const char *text, int invert);

/* printf-style row. Returns clipped length drawn. */
int lcd_ui_rowf(int row, int invert, const char *fmt, ...);

/* Status-bar helpers: row 0 headline, row 7 footer. */
void lcd_ui_headline(const char *text);
void lcd_ui_footer(const char *text);

/* Filled rectangle. */
void lcd_ui_rect(int x, int y, int w, int h, uint16_t color);

/* Four-bar Wi-Fi indicator, bottom-left at (x,y+h). Bars light up and
 * colour by RSSI (green >=-60, light green >=-70, yellow >=-80, red
 * below); all bars dim when !connected. 22x14 px. */
void lcd_ui_wifi_bars(int x, int y, int rssi, bool connected);

/* Bluetooth rune, 9x15 px, top-left at (x,y). */
void lcd_ui_bt_rune(int x, int y, uint16_t color);

/* Transfer indicator: up/down arrows + percent text, 1x font.
 * percent < 0 draws the arrows only (busy, size unknown). */
void lcd_ui_sync(int x, int y, uint16_t color, int percent);

/* Horizontal progress bar with 1-px outline. */
void lcd_ui_progress(int x, int y, int w, int h, int percent, uint16_t color);

/* Full-screen centre display (PINs, passwords): title 1x on top,
 * `code` at 4x in the middle, hint 1x at the bottom. */
void lcd_ui_pin_screen(const char *title, const char *code, const char *hint);

/* Block until the last queued SPI color transfer has drained — callers
 * that reuse a pixel buffer must wait before rewriting it. */
void board_lcd_wait_done(void);

#ifdef __cplusplus
}
#endif
