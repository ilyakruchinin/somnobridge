idf_component_register(
    SRCS journal.c
    INCLUDE_DIRS include
    REQUIRES nvs_flash settings esp_timer
)
