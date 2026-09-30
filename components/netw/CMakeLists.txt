idf_component_register(
    SRCS "wifi_mgr.c" "setup_portal.c" "dns_server.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_wifi esp_event esp_netif esp_http_server nvs_flash
             esp_timer json settings journal
    PRIV_REQUIRES mbedtls
)
