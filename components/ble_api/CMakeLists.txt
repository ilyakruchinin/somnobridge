idf_component_register(
    SRCS "ble_svc.c" "ble_proto.c" "ble_ops.c" "stream_codec.c"
    INCLUDE_DIRS "include"
    REQUIRES bt nvs_flash esp_timer esp_rom json mbedtls settings
             catalog scheduler ezshare netw board drift journal
)
