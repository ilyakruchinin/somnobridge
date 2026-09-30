idf_component_register(
    SRCS "settings.c"
    INCLUDE_DIRS "include"
    REQUIRES nvs_flash efuse mbedtls nvs_sec_provider
)
