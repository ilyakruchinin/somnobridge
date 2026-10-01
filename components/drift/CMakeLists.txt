idf_component_register(
    SRCS drift.c edf_hdr.c
    INCLUDE_DIRS include
    REQUIRES esp_timer nvs_flash settings esp_rom ezshare catalog
)
