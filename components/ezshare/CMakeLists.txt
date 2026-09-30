idf_component_register(
    SRCS "ezs_parse.c" "ezs_path.c" "ezshare.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_http_client esp_timer esp_event settings
)
