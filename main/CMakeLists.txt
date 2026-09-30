idf_component_register(
    SRCS "app_main.c"
    INCLUDE_DIRS "."
    REQUIRES board settings netw ezshare catalog scheduler ble_api journal drift
)
