/*
 * SomnoPal — board support (LCD + BOOT button)
 * Copyright (C) 2026 Ilya Kruchinin <https://github.com/ilyakruchinin>
 * GPLv3+ — see LICENSE and NOTICE for the Section 7(b) attribution term.
 */
#include "board.h"

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_check.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_st7789.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "board";

static esp_lcd_panel_handle_t s_panel;
static int64_t s_btn_down_us = -1;

/* Debounce/hold timing (spec §2.1) */
#define BTN_DEBOUNCE_US     (30 * 1000)
#define BTN_SHORT_MAX_US    (1000 * 1000)
#define BTN_LONG_US         (3000 * 1000)
#define POLL_PERIOD_US      (20 * 1000)

esp_err_t board_init(void)
{
    /* Backlight: plain GPIO, active-high */
    gpio_config_t bl = {
        .pin_bit_mask = 1ULL << BOARD_PIN_LCD_BL,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&bl), TAG, "bl cfg");
    gpio_set_level(BOARD_PIN_LCD_BL, 0);

    /* BOOT button: input, pull-up, active low */
    gpio_config_t btn = {
        .pin_bit_mask = 1ULL << BOARD_PIN_BOOT,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&btn), TAG, "btn cfg");

    /* SPI2 host for the LCD */
    spi_bus_config_t bus = {
        .mosi_io_num = BOARD_PIN_LCD_MOSI,
        .miso_io_num = -1,
        .sclk_io_num = BOARD_PIN_LCD_SCLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = BOARD_LCD_HRES * BOARD_LCD_VRES * 2 + 8,
        .flags = SPICOMMON_BUSFLAG_MASTER,
    };
    ESP_RETURN_ON_ERROR(spi_bus_initialize(SPI2_HOST, &bus, SPI_DMA_CH_AUTO),
                        TAG, "spi bus");

    esp_lcd_panel_io_spi_config_t io_cfg = {
        .cs_gpio_num = BOARD_PIN_LCD_CS,
        .dc_gpio_num = BOARD_PIN_LCD_DC,
        .spi_mode = 0,
        .pclk_hz = 40 * 1000 * 1000,
        .trans_queue_depth = 8,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
    };
    esp_lcd_panel_io_handle_t io;
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_spi(SPI2_HOST, &io_cfg, &io),
                        TAG, "lcd io");

    esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num = BOARD_PIN_LCD_RST,
        .color_space = ESP_LCD_COLOR_SPACE_RGB,
        .bits_per_pixel = 16,
        /* ST7789 on this board needs RGB->BGR and a 52/40 offset; the
         * vendor panel handles MADCTL — gap/offsets set below. */
        .flags = { .reset_active_high = 0 },
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_st7789(io, &panel_cfg, &s_panel),
                        TAG, "lcd panel");

    esp_lcd_panel_reset(s_panel);
    esp_lcd_panel_init(s_panel);
    /* Native portrait scan: 135x240 window at (52,40) in panel RAM,
     * mirrored on both axes, colors inverted (board vendor config). */
    esp_lcd_panel_mirror(s_panel, true, true);
    esp_lcd_panel_invert_color(s_panel, true);
    esp_lcd_panel_disp_on_off(s_panel, true);

    /* ST7789 RAM survives resets — clear the whole 240x320 buffer so
     * stale frames from previous boots don't ghost through the window. */
    {
        static uint16_t black[240 * 16];
        for (int y = 0; y < 320; y += 16) {
            esp_lcd_panel_draw_bitmap(s_panel, 0, y, 240, y + 16, black);
        }
    }
    esp_lcd_panel_set_gap(s_panel, 52, 40);

    board_backlight_set(30);
    ESP_LOGI(TAG, "board ready (lcd %ux%u)", BOARD_LCD_HRES, BOARD_LCD_VRES);
    return ESP_OK;
}

void board_backlight_set(uint8_t percent)
{
    gpio_set_level(BOARD_PIN_LCD_BL, percent > 0);
    /* TODO: LEDC PWM for intermediate brightness — v1 binary on/off. */
}

board_btn_event_t board_button_poll(void)
{
    static int64_t s_last_raw_low = -1;
    static bool s_debounced_down = false;
    static bool s_long_fired = false;

    bool raw = gpio_get_level(BOARD_PIN_BOOT) == 0;
    int64_t now = esp_timer_get_time();

    if (raw && !s_debounced_down) {
        if (s_last_raw_low < 0) {
            s_last_raw_low = now;
        } else if (now - s_last_raw_low >= BTN_DEBOUNCE_US) {
            s_debounced_down = true;
            s_btn_down_us = s_last_raw_low;
            s_long_fired = false;
        }
    } else if (!raw) {
        if (s_debounced_down) {
            int64_t held = now - s_btn_down_us;
            s_debounced_down = false;
            s_btn_down_us = -1;
            s_last_raw_low = -1;
            if (held >= BTN_LONG_US) {
                return BOARD_BTN_LONG;
            }
            if (held < BTN_SHORT_MAX_US) {
                return BOARD_BTN_SHORT;
            }
            return BOARD_BTN_NONE;   /* dead zone 1..3 s */
        }
        s_last_raw_low = -1;
    }
    if (s_debounced_down && !s_long_fired &&
        now - s_btn_down_us >= BTN_LONG_US) {
        s_long_fired = true;    /* for board_button_held_long() visibility */
    }
    return BOARD_BTN_NONE;
}

bool board_button_held_long(void)
{
    return s_btn_down_us >= 0 &&
           esp_timer_get_time() - s_btn_down_us >= BTN_LONG_US;
}

/* Raw framebuffer access used by lcd_ui.c */
esp_lcd_panel_handle_t board_lcd_panel(void)
{
    return s_panel;
}
