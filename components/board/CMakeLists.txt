idf_component_register(
    SRCS "board.c" "lcd_ui.c" "font8x16.c"
    INCLUDE_DIRS "include"
    REQUIRES driver esp_lcd esp_timer freertos
)
